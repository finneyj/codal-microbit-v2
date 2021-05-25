/*
The MIT License (MIT)

Copyright (c) 2020 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "MicroBitLog.h"
#include "CodalDmesg.h"
#include <new>

using namespace codal;

static ManagedString padString(ManagedString s, int digits)
{
    ManagedString zero = "0";
    while(s.length() != digits)
        s = zero + s;

    return s;
}

static void writeNum(char *buf, uint32_t n)
{
    int i = 0;
    int sh = 28;
    while (sh >= 0)
    {
        int d = (n >> sh) & 0xf;
        buf[i++] = d > 9 ? 'A' + d - 10 : '0' + d;
        sh -= 4;
    }
    buf[i] = 0;
}

/**
 * Constructor.
 */
MicroBitLog::MicroBitLog(MicroBitUSBFlashManager &flash, int journalPages) : flash(flash), cache(flash, CONFIG_MICROBIT_LOG_CACHE_BLOCK_SIZE, 4)
{
    this->journalPages = journalPages;
    this->status = 0;
    this->journalHead = 0;
    this->startAddress = 0;
    this->journalStart = 0;
    this->dataStart = 0;
    this->headingStart = 0;
    this->headingLength = 0;
    this->headingCount = 0;
    this->logEnd = 0;
    this->headingsChanged = false;
    this->rowData = NULL;
    this->timeStampFormat = TimeStampFormat::None;

    // Determine if the flash storage is already confgured as a valid log store..
    // If so, load the meta data. If not, reset it.
    //init();
}

/**
 * Attempt to load an exisitng filesystem, or format a new one if not found.
 */
void MicroBitLog::init()
{
    // If we're already initialized, do nothing. 
    if (status & MICROBIT_LOG_STATUS_INITIALIZED)
        return;

    if (isPresent())
    {
        // We have a valid file system.
        JournalEntry j;
        journalPages = (dataStart - startAddress) / flash.getPageSize() - 1;
        journalHead = journalStart;
        dataEnd = dataStart;

        // Load the last entry in the journal.
        uint32_t journalEntryAddress = journalHead;
        bool valid = false;

        while(journalEntryAddress < dataStart)
        {
            cache.read(journalEntryAddress, j.length, MICROBIT_LOG_JOURNAL_ENTRY_SIZE);

            // If we have a valid reading follwed by an unused entry, we're done.
            if (j.containsOnly(0xFF) && valid)
                break;

            // Parse valid entries. We continue processing to the last valid entry, just in case.
            if (!j.containsOnly(0x00))
            {
                journalHead = journalEntryAddress;
                dataEnd = dataStart + strtoul(j.length, NULL, 16);
                valid = true;
            }

            journalEntryAddress += MICROBIT_LOG_JOURNAL_ENTRY_SIZE;
        }

        // Walk the page indicated by dataEnd, and increment until an unused byte (0xFF) is found.
        uint8_t d = 0;
        while(dataEnd < logEnd)
        {
            cache.read(dataEnd, &d, 1);
            if (d == 0xFF)
                break;

            dataEnd++;
        }

        // Determine if we have any column headers defined
        // If so, parse them.
        uint32_t start = startAddress + sizeof(MicroBitLogMetaData);
        char c;
        
        // Skip any leading zeroes (erased old data)
        cache.read(start, &c, 1);
        while(c == 0)
        {
            start++;
            cache.read(start, &c, 1);
        }

        // Read read until we see a 0xFF character (unused memory)
        uint32_t end = start;
        while(c != 0xff)
        {
            end++;
            cache.read(end, &c, 1);
        }

        headingLength = (int)(end-start);

        // If we have a valid set of headers recorded, parse them in.
        if (headingLength > 0)
        {
            headingStart = start;
            headingCount = 0;

            char *headers = (char *) malloc(headingLength);
            cache.read(start, headers, headingLength);

            // Count the number of comma separated headers.
            // Also terminate each entry as a string as we go.
            for (uint32_t i=0; i<headingLength; i++)
            {
                if (headers[i] == ',' || headers[i] == '\n')
                {
                    headers[i] = 0;
                    headingCount++;
                }
            }

            // Allocate a RAM buffer to hold key/value pairs matching those defined
            rowData = (ColumnEntry *) malloc(sizeof(ColumnEntry) * headingCount);

            // Populate each entry.
            int i=0;
            for (uint32_t h=0; h<headingCount; h++)
            {
                new (&rowData[h]) ColumnEntry;
                rowData[h].key = ManagedString(&headers[i]);
                i = i + rowData[h].key.length() + 1;
            }

            free(headers);
        }

        // We may be full here, but this is still a valid state.
        status |= MICROBIT_LOG_STATUS_INITIALIZED;
        return;
    }

    // No valid file system found. Reformat the physical medium.
    clear();
}


/**
 * Reset all data stored in persistent storage.
 */
void MicroBitLog::clear(bool fullErase)
{
    mutex.wait();

    // Calculate where our metadata should start.
    startAddress = sizeof(header) % flash.getPageSize() == 0 ? sizeof(header) : (1+(sizeof(header) / flash.getPageSize())) * flash.getPageSize();
    journalPages = CONFIG_MICROBIT_LOG_JOURNAL_PAGES;
    journalStart = startAddress + flash.getPageSize();
    journalHead = journalStart;
    dataStart = journalStart + journalPages*flash.getPageSize();
    dataEnd = dataStart;
    logEnd = flash.getFlashEnd() - flash.getPageSize() - sizeof(uint32_t);
    status = 0;
    
    // Remove any cached state around column headings
    headingsChanged = false;
    headingStart = 0;
    headingCount = 0;
    headingLength = 0;

    if (rowData)
    {
        free(rowData);
        rowData = NULL;
    }

    // Erase block associated with the FULL indicator. We don't perform a pag eerase here to reduce flash wear.
    uint32_t zero = 0x00000000;
    flash.write(logEnd, &zero, 1);

    // Erase all pages associated with the header, all meta data and the first page of data storage.
    cache.clear();
    for (uint32_t p = flash.getFlashStart(); p <= (fullErase ? logEnd : dataStart); p += flash.getPageSize())
        flash.erase(p);

    // Serialise and write header (if we have one)
    // n.b. we use flash.write() here to avoid unecessary preheating of the cache.
    flash.write(flash.getFlashStart(), (uint32_t *)header, sizeof(header)/4);

    // Generate and write FS metadata
    memcpy(metaData.version, MICROBIT_LOG_VERSION, 18);
    memcpy(metaData.dataStart, "0x00000000\n", 11);
    memcpy(metaData.logEnd, "0x00000000\n", 11);

    writeNum(metaData.dataStart+2, dataStart);
    writeNum(metaData.logEnd+2, logEnd);

    cache.write(startAddress, &metaData, sizeof(metaData));

    // Record that the log is empty
    JournalEntry je;
    cache.write(journalHead, &je, MICROBIT_LOG_JOURNAL_ENTRY_SIZE);

    // Update physical file size and visibility information.
    MicroBitUSBFlashConfig config;
    config.fileName = "MY_DATA.HTM";
    config.fileSize = flash.getFlashEnd() - flash.getFlashStart() - flash.getPageSize();
    config.visible = true;
    
    flash.setConfiguration(config, true);
    flash.remount();

    status |= MICROBIT_LOG_STATUS_INITIALIZED;

    mutex.notify();
}

/**
 * Determines the format of the timestamp data to be added (if any).
 * If requested, time stamps will be automatically added to each row of data
 * as an integer value rounded down to the unit specified.
 * 
 * @param format The format of timestamp to use. 
 */
void MicroBitLog::setTimeStamp(TimeStampFormat format)
{
    init();
    this->timeStampFormat = format;

    // If we do not have a timestamp column associated with the requested time unit, create one.
    ManagedString units;

    switch (format) {
    case TimeStampFormat::None:
        return;

    case TimeStampFormat::Milliseconds:
        units = "milliseconds";
        break;

    case TimeStampFormat::Seconds:
        units = "seconds";
        break;

    case TimeStampFormat::Minutes:
        units = "minutes";
        break;

    case TimeStampFormat::Hours:
        units = "hours";
        break;

    case TimeStampFormat::Days:
        units = "hours";
        break;
    }

    timeStampHeading = "Time (" + units + ")";

    // Attempt to add the column, if it does not already exist.
    addHeading(timeStampHeading);
}

/**
 * Creates a new row in the log, ready to be populated by logData()
 * 
 * @return DEVICE_OK on success.
 */
int MicroBitLog::beginRow()
{
    init();

    // If beginRow is called during an open transaction, implicity perform an endRow before proceeding.
    if (status & MICROBIT_LOG_STATUS_ROW_STARTED)
        endRow();

    // Reset all values, ready to populate with a new row.
    for (uint32_t i=0; i<headingCount; i++)
        rowData[i].value = ManagedString();

    // indicate that we've started a new row.
    status |= MICROBIT_LOG_STATUS_ROW_STARTED;

    return DEVICE_OK;
}

/**
 * Populates the current row with the given key/value pair.
 * @param key the name of the key column) to set.
 * @param value the value to insert 
 * 
 * @return DEVICE_OK on success.
 */
int MicroBitLog::logData(const char *key, const char *value)
{
    return logData(ManagedString(key), ManagedString(value));
}

/**
 * Populates the current row with the given key/value pair.
 * @param key the name of the key column) to set.
 * @param value the value to insert 
 *
 * @return DEVICE_OK on success.
 */
int MicroBitLog::logData(ManagedString key, ManagedString value)
{
    // Perform lazy instatiation if necessary.
    init();

    // If logData is called before explicitly beginning a row, do so implicitly.
    if (!(status & MICROBIT_LOG_STATUS_ROW_STARTED))
        beginRow();

    ManagedString k = cleanBuffer(key.toCharArray(), key.length());
    ManagedString v = cleanBuffer(value.toCharArray(), value.length());

    if (k.length())
        key = k;

    if (v.length())
        value = v;

    // Add the given key/value pair into our cumulative row data. 
    bool added = false;
    for (uint32_t i=0; i<headingCount; i++)
    {
        if(rowData[i].key == key)
        {
            rowData[i].value = value;
            added = true;
            break;
        }
    }

    // If the requested heading is not available, add it.
    if (!added)
        addHeading(key, value);

    return DEVICE_OK;
}

/**
 * Complete a row in the log, and pushes to persistent storage.
 * @return DEVICE_OK on success.
 */
int MicroBitLog::endRow()
{
    if (!(status & MICROBIT_LOG_STATUS_ROW_STARTED))
        return DEVICE_INVALID_STATE;

    init();

    // Insert timestamp field if requested.
    if (timeStampFormat != TimeStampFormat::None)
    {
        // handle 32 bit overflow and fractional components of timestamp
        CODAL_TIMESTAMP t = system_timer_current_time() / (CODAL_TIMESTAMP)timeStampFormat;
        int billions = t / (CODAL_TIMESTAMP) 1000000000;
        int units = t % (CODAL_TIMESTAMP) 1000000000;
        int fraction = 0;

        if ((int)timeStampFormat > 1)
        {
            fraction = units % 100;
            units = units / 100;
            billions = billions / 100;
        }

        ManagedString u(units);
        ManagedString f(fraction);
        ManagedString s;
        f = padString(f, 2);

        if (billions)
        {
            s = s + billions;
            u = padString(u, 9);
        }

        s = s + u;

        // Add two decimal places for anything other than milliseconds.
        if ((int)timeStampFormat > 1)
            s = s + "." + f;

        logData(timeStampHeading, s);
    }

    // If new columns have been added since the last row, update persistent storage accordingly.
    ManagedString sep = ",";

    if (headingsChanged)
    {
        // If this is the first time we have logged any headings, place them just after the metadata block
        if (headingStart == 0)
            headingStart = startAddress + sizeof(MicroBitLogMetaData);

        // create new headers
        ManagedString h;
        ManagedBuffer zero(headingLength);

        for (uint32_t i=0; i<headingCount;i++)
        {
            h = h + rowData[i].key;
            if (i + 1 != headingCount)
                h = h + sep;
        }
        h = h + "\n";
        
        cache.write(headingStart, &zero[0], headingLength);
        headingStart += headingLength;
        cache.write(headingStart, h.toCharArray(), h.length());

        logString(h);

        headingsChanged = false;
    }

    // Serialize data to CSV
    ManagedString row;
    bool empty = true;

    for (uint32_t i=0; i<headingCount;i++)
    {
        row = row + rowData[i].value;
         
        if (rowData[i].value.length())
            empty = false;

        if (i + 1 != headingCount)
            row = row + sep;
    }
    row = row + "\n";

    if (!empty)
        logString(row);

    status &= ~MICROBIT_LOG_STATUS_ROW_STARTED;

    if (status & MICROBIT_LOG_STATUS_FULL)
        return DEVICE_NO_RESOURCES;

    return DEVICE_OK;
}

/**
 * Clean the given buffer of invalid LogFS symbols ("-->" and optionally ",\t\n")
 *
 * @param s the data to clean
 * @param len the number of characters to clean
 * @param removeSeperators if set to false, only "-->" symbols are erased, otherwise ",\t\n" characters are also removed.
 * @return a cleaned version of the string supplied, if any changes are necessary. Otherwise, an empty string is returned.
 */
ManagedString MicroBitLog::cleanBuffer(const char *s, int len, bool removeSeparators)
{
    ManagedString out;

    for (int i=0; i<len; i++)
    {
        if (i+2 < len && s[i] == '-' && s[i+1] == '-' && s[i+2] == '>')
        {
            if (out.length() == 0)
                out = ManagedString(s, len);
            *(char *)(out.toCharArray()+i) = CONFIG_MICROBIT_LOG_INVALID_CHAR_VALUE;
            *(char *)(out.toCharArray()+i+1) = CONFIG_MICROBIT_LOG_INVALID_CHAR_VALUE;
            *(char *)(out.toCharArray()+i+2) = CONFIG_MICROBIT_LOG_INVALID_CHAR_VALUE;
        }

        if (s[i] == '\t' || (removeSeparators && (s[i] == ',' || s[i] == '\n')))
        {
            if (out.length() == 0)
                out = ManagedString(s, len);
            *(char *)(out.toCharArray()+i) = CONFIG_MICROBIT_LOG_INVALID_CHAR_VALUE;
        }
    }
    return out;
}

/**
 * Inject the given row into the log as text, ignoring key/value pairs.
 * @param s the string to inject.
 */
int MicroBitLog::logString(const char *s)
{
    mutex.wait();
    
    init();

    uint32_t oldDataEnd = dataEnd;
    uint32_t l = strlen(s);
    const char *data = s;

    // If we can't write a whole line of data, then treat the log as full.
    if (l > logEnd - dataEnd)
    {
        if (!(status & MICROBIT_LOG_STATUS_FULL))
        {
            cache.write(logEnd+1, "FUL", 3);
            status |= MICROBIT_LOG_STATUS_FULL;
        }
        mutex.notify();
        return DEVICE_NO_RESOURCES;
    }

    ManagedString cleaned = cleanBuffer(data, l, false);
    if (cleaned.length())
        data = cleaned.toCharArray();

    while (l > 0)
    {
        uint32_t spaceOnPage = flash.getPageSize() - (dataEnd % flash.getPageSize());
        //DMESG("SPACE_ON_PAGE: %d", spaceOnPage);
        int lengthToWrite = min(l, spaceOnPage);

        // If we're going to fill (or overspill) the current page, erase the next one ready for use.
        if (spaceOnPage <= l && dataEnd+spaceOnPage < logEnd)
        {
            uint32_t nextPage = ((dataEnd / flash.getPageSize()) + 1) * flash.getPageSize();

            //DMESG("   ERASING PAGE %p", nextPage);
            flash.erase(nextPage);
        }

        // Perform a write through cache update
        //DMESG("   WRITING [ADDRESS: %p] [LENGTH: %d] ", dataEnd, lengthToWrite);
        cache.write(dataEnd, data, lengthToWrite);

        // move on pointers
        dataEnd += lengthToWrite;
        data += lengthToWrite;
        l -= lengthToWrite;
    }

    // Write a new entry into the log journal if we crossed a cache block boundary
    if ((dataEnd / CONFIG_MICROBIT_LOG_CACHE_BLOCK_SIZE) != (oldDataEnd / CONFIG_MICROBIT_LOG_CACHE_BLOCK_SIZE))
    {
        uint32_t oldJournalHead = journalHead;
        oldDataEnd = dataEnd;

        // Record that we've moved on the journal log by one entry
        journalHead += MICROBIT_LOG_JOURNAL_ENTRY_SIZE;

        // If we've moved onto another page, ensure it is erased.
        if (journalHead % flash.getPageSize() == 0)
        {
            //DMESG("JOURNAL PAGE BOUNDARY: %p", journalHead);
            // If we've rolled over the last page, cycle around.
            if (journalHead == dataStart)
            {
                //DMESG("JOURNAL WRAPAROUND");
                journalHead = journalStart;
            }

            //DMESG("ERASING JOURNAL PAGE: %p", journalHead);
            cache.erase(journalHead);
            flash.erase(journalHead);
        }

        // Write journal entry
        JournalEntry je;
        writeNum(je.length, ((dataEnd-dataStart) / CONFIG_MICROBIT_LOG_CACHE_BLOCK_SIZE) * CONFIG_MICROBIT_LOG_CACHE_BLOCK_SIZE);
        cache.write(journalHead, &je, MICROBIT_LOG_JOURNAL_ENTRY_SIZE);

        // Invalidate the old one
        JournalEntry empty;
        empty.clear();
        //DMESG("   INVALIDATING: %p", oldJournalHead);
        cache.write(oldJournalHead, &empty, MICROBIT_LOG_JOURNAL_ENTRY_SIZE);
    }

    mutex.notify();

    // Return NO_RESOURCES if we ran out of FLASH space.
    if (l == 0)
        return DEVICE_OK;
    else
        return DEVICE_NO_RESOURCES;
}

/**
 * Inject the given row into the log as text, ignoring key/value pairs.
 * @param s the string to inject.
 */
int MicroBitLog::logString(ManagedString s)
{
    logString(s.toCharArray());
    return DEVICE_OK;
}

/**
 * Add the given heading to the list of headings in use. If the heading already exists,
 * this method has no effect.
 * 
 * @param heading the heading to add
 */
void MicroBitLog::addHeading(ManagedString key, ManagedString value)
{
    for (uint32_t i=0; i<headingCount; i++)
        if (rowData[i].key == key)
            return;

    ColumnEntry* newRowData = (ColumnEntry *) malloc(sizeof(ColumnEntry) * (headingCount+1));

    for (uint32_t i=0; i<headingCount; i++)
    {
        new (&newRowData[i]) ColumnEntry;
        newRowData[i].key = rowData[i].key;
        newRowData[i].value = rowData[i].value;
        rowData[i].key = ManagedString::EmptyString;
        rowData[i].value = ManagedString::EmptyString;
    }   
    
    if (rowData)
        free(rowData);

    new (&newRowData[headingCount]) ColumnEntry;
    newRowData[headingCount].key = key;
    newRowData[headingCount].value = value;
    headingCount++;

    rowData = newRowData;
    headingsChanged = true;
}

/**
 * Marks an existing Log as invalid. The log will be cleared with the default settings the next time
 * a user attempts to use it. If no valid log is present, this method has no effect.
 */
void MicroBitLog::invalidate()
{
    DMESGF("LOG_FS: INVALIDATING");
    if (isPresent())
    {
        MicroBitLogMetaData m;
        memclr(&m, sizeof(MicroBitLogMetaData));

        // Erase the LogFS metadata and trailing FULL indicator.
        flash.write(startAddress, (uint32_t *) &m, sizeof(MicroBitLogMetaData)/4);
        flash.write(logEnd, (uint32_t *) &m, 1);
    }

    status &= ~MICROBIT_LOG_STATUS_INITIALIZED; 
}

/**
 * Determines if a MicroMitLogFS header is present.
 *
 * @return true if a MICROBIT_LOG_VERSION string is present at the expected location, false otherwise.
 */
bool MicroBitLog::isPresent()
{
    // Fast path if we;re already initialized.
    if (status & MICROBIT_LOG_STATUS_INITIALIZED)
        return true;

    // Calculate where our metadata should start, and load the data.
    startAddress = sizeof(header) % flash.getPageSize() == 0 ? sizeof(header) : (1+(sizeof(header) / flash.getPageSize())) * flash.getPageSize();

    // Read the metadata area from flash memory.
    // n.b. we do this using a direct read (rather than via the cache) to avoid preheating the cache with potentially useless data.
    flash.read((uint32_t *)&metaData, startAddress, sizeof(metaData)/4);

    // Ensure data strings are terminated
    metaData.dataStart[10] = 0;
    metaData.logEnd[10] = 0;
    metaData.version[17] = 0;

    // Determine if the FS looks valid.
    dataStart = strtoul(metaData.dataStart, NULL, 16);
    logEnd = strtoul(metaData.logEnd, NULL, 16);
    journalStart = startAddress + flash.getPageSize();

    // Perform some basic validation checks. Load in the state of the file system if things look OK.
    return (dataStart >= startAddress + 2*flash.getPageSize() && dataStart < logEnd && logEnd < flash.getFlashEnd() && memcmp(metaData.version, MICROBIT_LOG_VERSION, 17) == 0);
}

/**
 * Determines if this log is full
 *
 * @return true if the log is full, false otherwise.
 */
bool MicroBitLog::isFull()
{
    return (status & MICROBIT_LOG_STATUS_FULL);
}

/**
 * Destructor.
 */
MicroBitLog::~MicroBitLog()
{

}

const uint8_t MicroBitLog::header[2048] = {0x3C,0x68,0x74,0x6D,0x6C,0x20,0x69,0x64,0x20,0x3D,0x20,0x22,0x68,0x74,0x6D,0x22,0x3E,0x3C,0x68,0x65,0x61,0x64,0x3E,0x3C,0x73,0x74,0x79,0x6C,0x65,0x3E,0x68,0x74,0x6D,0x6C,0x2C,0x62,0x6F,0x64,0x79,0x7B,0x6D,0x61,0x72,0x67,0x69,0x6E,0x3A,0x31,0x65,0x6D,0x3B,0x66,0x6F,0x6E,0x74,0x2D,0x66,0x61,0x6D,0x69,0x6C,0x79,0x3A,0x73,0x61,0x6E,0x73,0x2D,0x73,0x65,0x72,0x69,0x66,0x7D,0x74,0x61,0x62,0x6C,0x65,0x7B,0x62,0x6F,0x72,0x64,0x65,0x72,0x2D,0x63,0x6F,0x6C,0x6C,0x61,0x70,0x73,0x65,0x3A,0x63,0x6F,0x6C,0x6C,0x61,0x70,0x73,0x65,0x3B,0x77,0x69,0x64,0x74,0x68,0x3A,0x35,0x30,0x25,0x7D,0x74,0x64,0x2C,0x74,0x68,0x7B,0x62,0x6F,0x72,0x64,0x65,0x72,0x3A,0x31,0x70,0x78,0x20,0x73,0x6F,0x6C,0x69,0x64,0x20,0x23,0x64,0x64,0x64,0x3B,0x70,0x61,0x64,0x64,0x69,0x6E,0x67,0x3A,0x38,0x70,0x78,0x7D,0x74,0x72,0x3A,0x6E,0x74,0x68,0x2D,0x63,0x68,0x69,0x6C,0x64,0x28,0x65,0x76,0x65,0x6E,0x29,0x7B,0x62,0x61,0x63,0x6B,0x67,0x72,0x6F,0x75,0x6E,0x64,0x2D,0x63,0x6F,0x6C,0x6F,0x72,0x3A,0x23,0x66,0x32,0x66,0x32,0x66,0x32,0x7D,0x74,0x72,0x3A,0x68,0x6F,0x76,0x65,0x72,0x7B,0x62,0x61,0x63,0x6B,0x67,0x72,0x6F,0x75,0x6E,0x64,0x2D,0x63,0x6F,0x6C,0x6F,0x72,0x3A,0x23,0x64,0x64,0x64,0x7D,0x74,0x68,0x7B,0x70,0x61,0x64,0x64,0x69,0x6E,0x67,0x2D,0x74,0x6F,0x70,0x3A,0x31,0x32,0x70,0x78,0x3B,0x70,0x61,0x64,0x64,0x69,0x6E,0x67,0x2D,0x62,0x6F,0x74,0x74,0x6F,0x6D,0x3A,0x31,0x32,0x70,0x78,0x3B,0x74,0x65,0x78,0x74,0x2D,0x61,0x6C,0x69,0x67,0x6E,0x3A,0x6C,0x65,0x66,0x74,0x3B,0x62,0x61,0x63,0x6B,0x67,0x72,0x6F,0x75,0x6E,0x64,0x2D,0x63,0x6F,0x6C,0x6F,0x72,0x3A,0x23,0x34,0x63,0x61,0x66,0x35,0x30,0x3B,0x63,0x6F,0x6C,0x6F,0x72,0x3A,0x77,0x68,0x69,0x74,0x65,0x7D,0x3C,0x2F,0x73,0x74,0x79,0x6C,0x65,0x3E,0x3C,0x2F,0x68,0x65,0x61,0x64,0x3E,0x0D,0x0A,0x3C,0x48,0x32,0x3E,0x6D,0x69,0x63,0x72,0x6F,0x3A,0x62,0x69,0x74,0x20,0x44,0x61,0x74,0x61,0x20,0x4C,0x6F,0x67,0x3C,0x2F,0x48,0x32,0x3E,0x3C,0x62,0x6F,0x64,0x79,0x20,0x69,0x64,0x20,0x3D,0x20,0x22,0x62,0x6F,0x64,0x22,0x3E,0x3C,0x62,0x75,0x74,0x74,0x6F,0x6E,0x20,0x6F,0x6E,0x63,0x6C,0x69,0x63,0x6B,0x3D,0x27,0x64,0x6F,0x77,0x6E,0x6C,0x6F,0x61,0x64,0x5F,0x66,0x69,0x6C,0x65,0x28,0x22,0x6D,0x69,0x63,0x72,0x6F,0x62,0x69,0x74,0x2E,0x63,0x73,0x76,0x22,0x29,0x27,0x20,0x69,0x64,0x3D,0x22,0x64,0x6C,0x6F,0x61,0x64,0x22,0x3E,0x44,0x6F,0x77,0x6E,0x6C,0x6F,0x61,0x64,0x3C,0x2F,0x62,0x75,0x74,0x74,0x6F,0x6E,0x3E,0x3C,0x62,0x75,0x74,0x74,0x6F,0x6E,0x20,0x6F,0x6E,0x63,0x6C,0x69,0x63,0x6B,0x3D,0x27,0x6E,0x61,0x76,0x69,0x67,0x61,0x74,0x6F,0x72,0x2E,0x63,0x6C,0x69,0x70,0x62,0x6F,0x61,0x72,0x64,0x2E,0x77,0x72,0x69,0x74,0x65,0x54,0x65,0x78,0x74,0x28,0x6C,0x6F,0x67,0x44,0x61,0x74,0x61,0x2E,0x72,0x65,0x70,0x6C,0x61,0x63,0x65,0x28,0x2F,0x5C,0x2C,0x2F,0x67,0x69,0x2C,0x20,0x22,0x5C,0x74,0x22,0x29,0x29,0x27,0x3E,0x43,0x6F,0x70,0x79,0x3C,0x2F,0x62,0x75,0x74,0x74,0x6F,0x6E,0x3E,0x3C,0x62,0x75,0x74,0x74,0x6F,0x6E,0x20,0x6F,0x6E,0x63,0x6C,0x69,0x63,0x6B,0x3D,0x27,0x61,0x6C,0x65,0x72,0x74,0x28,0x22,0x50,0x6C,0x65,0x61,0x73,0x65,0x20,0x75,0x6E,0x70,0x6C,0x75,0x67,0x20,0x79,0x6F,0x75,0x72,0x20,0x6D,0x69,0x63,0x72,0x6F,0x3A,0x62,0x69,0x74,0x2C,0x20,0x74,0x68,0x65,0x6E,0x20,0x70,0x6C,0x75,0x67,0x20,0x69,0x74,0x20,0x62,0x61,0x63,0x6B,0x20,0x69,0x6E,0x20,0x61,0x6E,0x64,0x20,0x72,0x65,0x2D,0x6F,0x70,0x65,0x6E,0x20,0x74,0x68,0x69,0x73,0x20,0x66,0x69,0x6C,0x65,0x22,0x29,0x27,0x3E,0x55,0x70,0x64,0x61,0x74,0x65,0x20,0x44,0x61,0x74,0x61,0x3C,0x2F,0x62,0x75,0x74,0x74,0x6F,0x6E,0x3E,0x3C,0x62,0x75,0x74,0x74,0x6F,0x6E,0x20,0x6F,0x6E,0x63,0x6C,0x69,0x63,0x6B,0x3D,0x27,0x61,0x6C,0x65,0x72,0x74,0x28,0x22,0x59,0x6F,0x75,0x72,0x20,0x63,0x6F,0x64,0x65,0x20,0x6F,0x6E,0x20,0x74,0x68,0x65,0x20,0x6D,0x69,0x63,0x72,0x6F,0x3A,0x62,0x69,0x74,0x20,0x6D,0x75,0x73,0x74,0x20,0x63,0x6C,0x65,0x61,0x72,0x20,0x74,0x68,0x65,0x20,0x6C,0x6F,0x67,0x20,0x75,0x73,0x69,0x6E,0x67,0x20,0x74,0x68,0x65,0x20,0x5C,0x22,0x66,0x6F,0x72,0x6D,0x61,0x74,0x20,0x64,0x72,0x69,0x76,0x65,0x5C,0x22,0x20,0x63,0x6F,0x6D,0x6D,0x61,0x6E,0x64,0x22,0x29,0x27,0x3E,0x43,0x6C,0x65,0x61,0x72,0x20,0x6C,0x6F,0x67,0x3C,0x2F,0x62,0x75,0x74,0x74,0x6F,0x6E,0x3E,0x3C,0x74,0x61,0x62,0x6C,0x65,0x20,0x69,0x64,0x3D,0x22,0x64,0x61,0x74,0x61,0x2D,0x76,0x69,0x65,0x77,0x22,0x3E,0x3C,0x2F,0x74,0x61,0x62,0x6C,0x65,0x3E,0x0D,0x0A,0x3C,0x73,0x63,0x72,0x69,0x70,0x74,0x3E,0x76,0x61,0x72,0x20,0x6C,0x6F,0x67,0x44,0x61,0x74,0x61,0x3B,0x66,0x75,0x6E,0x63,0x74,0x69,0x6F,0x6E,0x20,0x73,0x74,0x61,0x72,0x74,0x28,0x29,0x7B,0x6C,0x65,0x74,0x20,0x73,0x74,0x75,0x66,0x66,0x3D,0x64,0x6F,0x63,0x75,0x6D,0x65,0x6E,0x74,0x2E,0x67,0x65,0x74,0x45,0x6C,0x65,0x6D,0x65,0x6E,0x74,0x42,0x79,0x49,0x64,0x28,0x22,0x68,0x74,0x6D,0x22,0x29,0x3B,0x6C,0x65,0x74,0x20,0x72,0x61,0x77,0x3D,0x28,0x73,0x74,0x75,0x66,0x66,0x2E,0x6F,0x75,0x74,0x65,0x72,0x48,0x54,0x4D,0x4C,0x29,0x2E,0x73,0x70,0x6C,0x69,0x74,0x28,0x22,0x3C,0x21,0x2D,0x2D,0x46,0x53,0x5F,0x53,0x54,0x41,0x52,0x54,0x22,0x29,0x5B,0x32,0x5D,0x3B,0x69,0x66,0x28,0x72,0x61,0x77,0x2E,0x73,0x75,0x62,0x73,0x74,0x72,0x69,0x6E,0x67,0x28,0x30,0x2C,0x31,0x37,0x29,0x3D,0x3D,0x22,0x55,0x42,0x49,0x54,0x5F,0x4C,0x4F,0x47,0x5F,0x46,0x53,0x5F,0x56,0x5F,0x30,0x30,0x31,0x22,0x29,0x7B,0x6C,0x65,0x74,0x20,0x6C,0x3D,0x30,0x3B,0x6C,0x65,0x74,0x20,0x6C,0x6F,0x67,0x45,0x6E,0x64,0x3D,0x70,0x61,0x72,0x73,0x65,0x49,0x6E,0x74,0x28,0x72,0x61,0x77,0x2E,0x73,0x75,0x62,0x73,0x74,0x72,0x69,0x6E,0x67,0x28,0x31,0x38,0x2C,0x32,0x39,0x29,0x2C,0x31,0x36,0x29,0x3B,0x6C,0x65,0x74,0x20,0x64,0x61,0x74,0x61,0x53,0x74,0x61,0x72,0x74,0x3D,0x70,0x61,0x72,0x73,0x65,0x49,0x6E,0x74,0x28,0x72,0x61,0x77,0x2E,0x73,0x75,0x62,0x73,0x74,0x72,0x69,0x6E,0x67,0x28,0x32,0x39,0x2C,0x34,0x30,0x29,0x2C,0x31,0x36,0x29,0x2D,0x32,0x30,0x34,0x38,0x3B,0x6C,0x65,0x74,0x20,0x6A,0x6F,0x75,0x72,0x6E,0x61,0x6C,0x3D,0x31,0x30,0x32,0x34,0x3B,0x6C,0x65,0x74,0x20,0x64,0x61,0x74,0x61,0x45,0x6E,0x64,0x3D,0x30,0x3B,0x77,0x68,0x69,0x6C,0x65,0x28,0x6A,0x6F,0x75,0x72,0x6E,0x61,0x6C,0x21,0x3D,0x64,0x61,0x74,0x61,0x53,0x74,0x61,0x72,0x74,0x29,0x7B,0x6C,0x65,0x74,0x20,0x72,0x3D,0x70,0x61,0x72,0x73,0x65,0x49,0x6E,0x74,0x28,0x72,0x61,0x77,0x2E,0x73,0x75,0x62,0x73,0x74,0x72,0x69,0x6E,0x67,0x28,0x6A,0x6F,0x75,0x72,0x6E,0x61,0x6C,0x2C,0x6A,0x6F,0x75,0x72,0x6E,0x61,0x6C,0x2B,0x38,0x29,0x2C,0x31,0x36,0x29,0x3B,0x69,0x66,0x28,0x72,0x3D,0x3D,0x72,0x29,0x7B,0x64,0x61,0x74,0x61,0x45,0x6E,0x64,0x3D,0x64,0x61,0x74,0x61,0x53,0x74,0x61,0x72,0x74,0x2B,0x72,0x3B,0x62,0x72,0x65,0x61,0x6B,0x7D,0x6A,0x6F,0x75,0x72,0x6E,0x61,0x6C,0x2B,0x3D,0x38,0x7D,0x77,0x68,0x69,0x6C,0x65,0x28,0x72,0x61,0x77,0x2E,0x63,0x68,0x61,0x72,0x43,0x6F,0x64,0x65,0x41,0x74,0x28,0x64,0x61,0x74,0x61,0x45,0x6E,0x64,0x29,0x21,0x3D,0x31,0x36,0x30,0x29,0x64,0x61,0x74,0x61,0x45,0x6E,0x64,0x2B,0x2B,0x3B,0x6C,0x6F,0x67,0x44,0x61,0x74,0x61,0x3D,0x72,0x61,0x77,0x2E,0x73,0x75,0x62,0x73,0x74,0x72,0x69,0x6E,0x67,0x28,0x64,0x61,0x74,0x61,0x53,0x74,0x61,0x72,0x74,0x2C,0x64,0x61,0x74,0x61,0x45,0x6E,0x64,0x29,0x7D,0x6C,0x65,0x74,0x20,0x74,0x61,0x62,0x6C,0x65,0x3D,0x64,0x6F,0x63,0x75,0x6D,0x65,0x6E,0x74,0x2E,0x67,0x65,0x74,0x45,0x6C,0x65,0x6D,0x65,0x6E,0x74,0x42,0x79,0x49,0x64,0x28,0x22,0x64,0x61,0x74,0x61,0x2D,0x76,0x69,0x65,0x77,0x22,0x29,0x3B,0x6C,0x65,0x74,0x20,0x72,0x6F,0x77,0x73,0x3D,0x6C,0x6F,0x67,0x44,0x61,0x74,0x61,0x2E,0x73,0x70,0x6C,0x69,0x74,0x28,0x22,0x5C,0x6E,0x22,0x29,0x3B,0x66,0x6F,0x72,0x28,0x6C,0x65,0x74,0x20,0x69,0x3D,0x30,0x3B,0x69,0x3C,0x72,0x6F,0x77,0x73,0x2E,0x6C,0x65,0x6E,0x67,0x74,0x68,0x3B,0x69,0x2B,0x2B,0x29,0x7B,0x6C,0x65,0x74,0x20,0x63,0x65,0x6C,0x6C,0x73,0x3D,0x72,0x6F,0x77,0x73,0x5B,0x69,0x5D,0x2E,0x73,0x70,0x6C,0x69,0x74,0x28,0x22,0x2C,0x22,0x29,0x3B,0x69,0x66,0x28,0x63,0x65,0x6C,0x6C,0x73,0x2E,0x6C,0x65,0x6E,0x67,0x74,0x68,0x3E,0x31,0x29,0x7B,0x6C,0x65,0x74,0x20,0x72,0x6F,0x77,0x3D,0x74,0x61,0x62,0x6C,0x65,0x2E,0x69,0x6E,0x73,0x65,0x72,0x74,0x52,0x6F,0x77,0x28,0x2D,0x31,0x29,0x3B,0x66,0x6F,0x72,0x28,0x6C,0x65,0x74,0x20,0x6A,0x3D,0x30,0x3B,0x6A,0x3C,0x63,0x65,0x6C,0x6C,0x73,0x2E,0x6C,0x65,0x6E,0x67,0x74,0x68,0x3B,0x6A,0x2B,0x2B,0x29,0x7B,0x6C,0x65,0x74,0x20,0x63,0x65,0x6C,0x6C,0x3D,0x72,0x6F,0x77,0x2E,0x69,0x6E,0x73,0x65,0x72,0x74,0x43,0x65,0x6C,0x6C,0x28,0x2D,0x31,0x29,0x3B,0x63,0x65,0x6C,0x6C,0x2E,0x69,0x6E,0x6E,0x65,0x72,0x48,0x54,0x4D,0x4C,0x3D,0x63,0x65,0x6C,0x6C,0x73,0x5B,0x6A,0x5D,0x7D,0x7D,0x7D,0x7D,0x66,0x75,0x6E,0x63,0x74,0x69,0x6F,0x6E,0x20,0x64,0x6F,0x77,0x6E,0x6C,0x6F,0x61,0x64,0x5F,0x66,0x69,0x6C,0x65,0x28,0x61,0x29,0x7B,0x76,0x61,0x72,0x20,0x62,0x3D,0x6E,0x65,0x77,0x20,0x42,0x6C,0x6F,0x62,0x28,0x5B,0x6C,0x6F,0x67,0x44,0x61,0x74,0x61,0x5D,0x2C,0x7B,0x74,0x79,0x70,0x65,0x3A,0x22,0x74,0x65,0x78,0x74,0x2F,0x70,0x6C,0x61,0x69,0x6E,0x22,0x7D,0x29,0x3B,0x76,0x61,0x72,0x20,0x63,0x3D,0x64,0x6F,0x63,0x75,0x6D,0x65,0x6E,0x74,0x2E,0x63,0x72,0x65,0x61,0x74,0x65,0x45,0x6C,0x65,0x6D,0x65,0x6E,0x74,0x28,0x27,0x61,0x27,0x29,0x3B,0x63,0x2E,0x64,0x6F,0x77,0x6E,0x6C,0x6F,0x61,0x64,0x3D,0x61,0x3B,0x63,0x2E,0x68,0x72,0x65,0x66,0x3D,0x77,0x69,0x6E,0x64,0x6F,0x77,0x2E,0x55,0x52,0x4C,0x2E,0x63,0x72,0x65,0x61,0x74,0x65,0x4F,0x62,0x6A,0x65,0x63,0x74,0x55,0x52,0x4C,0x28,0x62,0x29,0x3B,0x63,0x2E,0x63,0x6C,0x69,0x63,0x6B,0x28,0x29,0x3B,0x63,0x2E,0x72,0x65,0x6D,0x6F,0x76,0x65,0x28,0x29,0x7D,0x76,0x61,0x72,0x20,0x73,0x74,0x75,0x66,0x66,0x3D,0x64,0x6F,0x63,0x75,0x6D,0x65,0x6E,0x74,0x2E,0x67,0x65,0x74,0x45,0x6C,0x65,0x6D,0x65,0x6E,0x74,0x42,0x79,0x49,0x64,0x28,0x22,0x62,0x6F,0x64,0x22,0x29,0x3B,0x73,0x74,0x75,0x66,0x66,0x2E,0x6F,0x6E,0x6C,0x6F,0x61,0x64,0x3D,0x73,0x74,0x61,0x72,0x74,0x3B,0x3C,0x2F,0x73,0x63,0x72,0x69,0x70,0x74,0x3E,0x0D,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x0D,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x0D,0x0A,0x3C,0x21,0x2D,0x2D,0x46,0x53,0x5F,0x53,0x54,0x41,0x52,0x54};