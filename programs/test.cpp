#include "../lib/zstd.h"
#include <stdio.h>
#include <assert.h>
#include <string>
#include <string_view>

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

int main(int argc, const char* argv[])
{
    std::string testdata;
    if (!readFile("test-data.json", testdata))
        error("test-data.json missing\n");
}
