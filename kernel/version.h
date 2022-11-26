#pragma once

#define major      0
#define minor      1
#define patch      0

#define ver(arg)   #arg
#define ver2(arg)  ver(arg)

#define major2     ver2(major)
#define minor2     ver2(minor)
#define patch2     ver2(patch)
#define dot        "."

#define VERSION    (major2 dot minor2 dot patch2)

