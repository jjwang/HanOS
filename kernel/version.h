/**-----------------------------------------------------------------------------

 @file    version.h
 @brief   Definition of version information
 @details
 @verbatim

  This file defines the version information for the HanOS Kernel. The version
  information is broken down into major, minor, and patch levels. These values
  are combined to form a version string that can be used to identify the
  specific version of the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define major      0
#define minor      1
#define patch      2

#define ver(arg)   #arg
#define ver2(arg)  ver(arg)

#define major2     ver2(major)
#define minor2     ver2(minor)
#define patch2     ver2(patch)
#define dot        "."

#define VERSION    (major2 dot minor2 dot patch2)

