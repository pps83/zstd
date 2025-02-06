#include "../lib/zstd.h"
#include <stdio.h>
#include <string>

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

int main(int argc, const char* argv[])
{
    std::string testdata;
    if (!readFile("test-data.json", testdata))
        error("test-data.json missing\n");
}
