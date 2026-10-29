#pragma once
#ifdef __ANDROID__
#include <android/log.h>
#define NFS_RUNTIME_LOG __android_log_print
#else
#include <cstdio>
#include <cstdarg>
#include <string>
constexpr int ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6;
// The Linux test host watches the log (tools/testhost: a script waits for a line); called after each is printed.
inline void (*runtimeLogObserver)(const std::string& line) = nullptr;
inline int runtimeLog(int, const char* tag, const char* format, ...) {
    va_list args, measure; va_start(args,format); va_copy(measure,args);
    const int length=std::vsnprintf(nullptr,0,format,measure); va_end(measure);
    std::string line(length>0?size_t(length):0,'\0');
    if(length>0)std::vsnprintf(line.data(),size_t(length)+1,format,args);
    va_end(args);
    std::fprintf(stderr,"[%s] %s\n",tag,line.c_str());
    if(runtimeLogObserver)runtimeLogObserver(line);
    return length;
}
#define NFS_RUNTIME_LOG runtimeLog
#endif
