#include "../lib/zstd.h"
#include <stdio.h>
#include <assert.h>
#include <string>
#include <string_view>
#include <utility>
#include <map>
#include <windows.h>

bool readFile(const char* fileName, std::string& fileData)
{
    fileData.clear();
    if (FILE* fl = fopen(fileName, "rb"))
    {
        setbuf(fl, nullptr);
        fseek(fl, 0, SEEK_END);
        int64_t length = _ftelli64(fl);
        fseek(fl, 0, SEEK_SET);
        size_t sz = (size_t)length;
        if (length > 0 && length == (int64_t)sz)
        {
            fileData.resize(sz);
            if (sz != fread(&fileData[0], 1, sz, fl))
                fileData.clear();
        }
        fclose(fl);
        return sz == fileData.size();
    }
    return false;
}

void error(std::string msg)
{
    fprintf(stderr, "%s", msg.c_str());
    fflush(stderr);
    abort();
}

#define ROUND128(n) (((n) + 512) & ~127) // add 512 bytes padding and round to 128 bytes

static int compress(std::string_view body, std::string& bodyCompressed, int compressionLevel,
    size_t(*compressFunc)(const void* body, size_t bodySize, void* data, size_t size, int compressionLevel))
{
    const size_t sz0 = bodyCompressed.size();
    const size_t maxDstSize = body.size() + std::max(body.size() / 3, (size_t)256);
    bodyCompressed.resize(ROUND128(sz0 + maxDstSize) - 1);
    const size_t compressedSize = compressFunc(body.data(), body.size(), &bodyCompressed[sz0], bodyCompressed.size() - sz0, compressionLevel);
    bodyCompressed.resize(sz0 + compressedSize);
    return compressedSize > 0 ? 0 : -1;
}

static int uncompress(std::string_view body, std::string& bodyUncompressed, size_t uncompressedSize,
    size_t(*uncompressFunc)(const void* body, size_t bodySize, void* data, size_t size))
{
    const size_t sz0 = bodyUncompressed.size();
    bodyUncompressed.resize(ROUND128(sz0 + uncompressedSize));
    const size_t size = uncompressFunc(body.data(), body.size(), &bodyUncompressed[sz0], uncompressedSize);
    bodyUncompressed.resize(sz0 + size);
    return size == 0 ? -1 : 0;
}

size_t compressZstd(const void* body, size_t bodySize, void* data, size_t size, int compressionLevel)
{
    assert(compressionLevel >= 1 && compressionLevel <= ZSTD_maxCLevel());
    assert(size >= ZSTD_compressBound(bodySize));
    size_t x = ZSTD_compress(data, size, body, bodySize, compressionLevel);
    assert(x <= size);
    return ZSTD_isError(x) ? size_t(0) : x;
}

size_t uncompressZstd(const void* body, size_t bodySize, void* data, size_t size)
{
    size_t x = ZSTD_decompress(data, size, body, bodySize);
    return ZSTD_isError(x) ? size_t(0) : x;
}

int compressZstd(std::string_view body, std::string& bodyCompressed, int compressionLevel)
{
    return compress(body, bodyCompressed, compressionLevel, compressZstd);
}
int uncompressZstd(std::string_view body, std::string& bodyUncompressed, size_t uncompressedSize)
{
    return uncompress(body, bodyUncompressed, uncompressedSize, uncompressZstd);
}

long long timeTicks()
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

long long ticksToMicro(long long x)
{
    static long long qpcFreq = 0;
    if (qpcFreq == 0)
    {
        LARGE_INTEGER n;
        QueryPerformanceFrequency(&n);
        qpcFreq = n.QuadPart;
    }
    if (qpcFreq == 10000000)
        return x / 10;
    long long x_sec = x / qpcFreq;
    long long x_rem = x % qpcFreq;
    return 1000000LL * x_sec + 1000000LL * x_rem / qpcFreq;
}

std::string fmtStr(const char* fmt, ...)
{
    char buf[1024];
    va_list argList;
    va_start(argList, fmt);
    vsnprintf(buf, 1000, fmt, argList);
    va_end(argList);
    return buf;
}

extern "C" void ZSTD_enable_cl();
extern "C" void ZSTD_enable_clang();

int main(int argc, const char* argv[])
{
    std::string testdata;
    if (!readFile("test-data.json", testdata))
        error("test-data.json missing\n");

    // thread to run on CPU 1
    SetThreadAffinityMask(GetCurrentThread(), 1 << 1);
    SetProcessPriorityBoost(GetCurrentProcess(), TRUE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const int N = 3;

    struct result
    {
        size_t compressedSize = 0;
        long long compressTicks = INT_MAX;
        long long uncompressTicks = INT_MAX;
    };

    std::map<std::string, std::map<int, result>> results;
    for (int type = 0; type < 2; ++type)
    {
        if (type == 0)
            ZSTD_enable_cl();
        else if (type == 1)
            ZSTD_enable_clang();

        static const int levels[] = {1, 5, 10};
        for (auto LEVEL : levels)
        {
            const char* codec = type == 0 ? "zstd" : "zstd-clang";
            auto& r = results[codec][LEVEL];
            for (int i = 0; i < 3; ++i)
            {
                std::string bodyCompressed;
                long long t0 = timeTicks();
                compressZstd(testdata, bodyCompressed, LEVEL);
                r.compressTicks = std::min(r.compressTicks, timeTicks() - t0);
                r.compressedSize = bodyCompressed.size();
                std::string body;
                for (int ii = 0; ii < N; ++ii) {
                    body.clear();
                    t0 = timeTicks();
                    uncompressZstd(bodyCompressed, body, testdata.size());
                    r.uncompressTicks = std::min(r.uncompressTicks, timeTicks() - t0);
                }
            }
        }
    }

    int64_t dticksAll_cl = 0, dticksAll_clang = 0;

    auto ticksTimeStr = [](int64_t t) { return fmtStr("%.2fus", ticksToMicro(t * 100) / 100.0); };
    for (const auto& [codec, v1] : results)
    {
        printf("%s:\n", codec.c_str());
        int64_t eticksAll = 0, dticksAll = 0;
        for (const auto& [level, res] : v1)
        {
            eticksAll += res.compressTicks;
            dticksAll += res.uncompressTicks;
            printf("L:%2d etime:%-11s (%zu, %.2f%%), dtime:%s\n", level, ticksTimeStr(res.compressTicks).c_str(),
                res.compressedSize, res.compressedSize * 100.0 / testdata.size(), ticksTimeStr(res.uncompressTicks).c_str());
        }
        printf("total etime:%s, total dtime:%s\n\n", ticksTimeStr(eticksAll).c_str(), ticksTimeStr(dticksAll).c_str());
        if (codec == "zstd-clang")
            dticksAll_clang = dticksAll;
        else
            dticksAll_cl = dticksAll;
    }

    printf("ZSTD_decompressSequences_body compiled with clang does zstd decompression %.02f%% %s\n",
        100.0 * std::abs(dticksAll_cl - dticksAll_clang) / dticksAll_cl,
        dticksAll_clang < dticksAll_cl ? "faster" : "slower");
}
