#ifndef _EMUCODES_H_
#define _EMUCODES_H_

enum crStatus {
    // Neutral
    CRS_NONE = 0, // Empty status
    
    // Positive
    CRS_FCOK = 0x10, // New file created succesfully
    CRS_RNOK = 0x20, // File renamed succesfully
    CRS_DLOK = 0x30, // File deleted succesfully
    CRS_DIOK = 0x40, // Correct disk info retreived
    
    // Errors
    // - Critical
    CRS_FDRE = 0xE0, // FAT driver error
    CRS_BFAT = 0xE1, // Bad filesystem
    CRS_DSIE = 0xE2, // FS space calc error
    CRS_RERR = 0xE3, // Read error
    CRS_WERR = 0xE4, // Write error
    CRS_NAUT = 0xE5, // No files to mount
    // - Recoverable
    CRS_IFIN = 0xE6, // Invalid file name
    CRS_IPAT = 0xE7, // Invalid path
    CRS_FAEX = 0xE8, // File already exists
    CRS_NESP = 0xE9, // Not enough space
    CRS_FINF = 0xEA, // File not found
    CRS_ACDN = 0xEB  // Access denied
};

const char* errMsgs[] = {
    "FAT driver error.",
    "Bad filesystem.",
    "FS space calc error.",
    "Read error.",
    "Write error.",
    "No files to mount.",
    "Invalid file name.",
    "Invalid path.",
    "File already exists.",
    "Not enough space.",
    "File not found.",
    "Access denied."
};

const char genErrMsg[] = "Unknown failure.";

#endif
