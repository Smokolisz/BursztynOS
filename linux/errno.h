#pragma once

namespace lx {

enum Errno : int {
    kEPERM = 1,
    kENOENT = 2,
    kESRCH = 3,
    kEINTR = 4,
    kEIO = 5,
    kEBADF = 9,
    kECHILD = 10,
    kEAGAIN = 11,
    kENOMEM = 12,
    kEACCES = 13,
    kEFAULT = 14,
    kEBUSY = 16,
    kEEXIST = 17,
    kENOTDIR = 20,
    kEISDIR = 21,
    kEINVAL = 22,
    kEMFILE = 24,
    kENOTTY = 25,
    kENOSPC = 28,
    kESPIPE = 29,
    kEROFS = 30,
    kEPIPE = 32,
    kERANGE = 34,
    kEDEADLK = 35,
    kENAMETOOLONG = 36,
    kENOSYS = 38,
    kENOTEMPTY = 39,
    kELOOP = 40,
    kENODATA = 61,
    kENOTSOCK = 88,
    kEOPNOTSUPP = 95,
    kEAFNOSUPPORT = 97,
    kETIMEDOUT = 110,
};

}
