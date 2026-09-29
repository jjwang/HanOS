/**-----------------------------------------------------------------------------

 @file    version.h
 @brief   Definition of version information
 @details
 @verbatim

  Exposes the kernel version as the VERSION string. The value comes from the
  top-level VERSION file, which the build passes in as VERSION_STRING; the
  fallback below keeps the header usable outside the build.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#ifndef VERSION_STRING
#define VERSION_STRING "0.0.0-dev"
#endif

#define VERSION    VERSION_STRING
