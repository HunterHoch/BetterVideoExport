#include "ImportHook.h"
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace ImportHook
{
    bool Patch(HMODULE imageModule,
               const char* dllName,
               const char* functionName,
               void* expectedAddress,
               void* replacement,
               void** originalAddress,
               std::string& detail)
    {
        if (!imageModule || !dllName || !functionName || !expectedAddress ||
            !replacement || !originalAddress)
        {
            detail = "invalid IAT hook arguments";
            return false;
        }

        auto* base = reinterpret_cast<std::uint8_t*>(imageModule);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 1024 * 1024)
        {
            detail = "invalid DOS header";
            return false;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            detail = "invalid PE64 header";
            return false;
        }
        const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
        const auto inImage = [imageSize](ULONGLONG rva, ULONGLONG bytes = 1) {
            return rva < imageSize && bytes <= imageSize - rva;
        };
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !directory.Size ||
            !inImage(directory.VirtualAddress, directory.Size))
        {
            detail = "main executable has no readable import directory";
            return false;
        }

        auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        const size_t maxDescriptors = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        for (size_t di = 0; di < maxDescriptors; ++di)
        {
            const IMAGE_IMPORT_DESCRIPTOR& descriptor = imports[di];
            if (!descriptor.Name && !descriptor.FirstThunk)
                break;
            if (!inImage(descriptor.Name) ||
                !inImage(descriptor.FirstThunk, sizeof(IMAGE_THUNK_DATA64)))
                continue;
            const char* importedDll = reinterpret_cast<const char*>(base + descriptor.Name);
            if (!std::memchr(importedDll, '\0', imageSize - descriptor.Name) ||
                _stricmp(importedDll, dllName) != 0)
                continue;

            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor.FirstThunk);
            const auto* names = descriptor.OriginalFirstThunk &&
                inImage(descriptor.OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64))
                ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor.OriginalFirstThunk)
                : nullptr;
            const size_t limit = (imageSize - descriptor.FirstThunk) / sizeof(IMAGE_THUNK_DATA64);
            const size_t nameLimit = names
                ? (imageSize - descriptor.OriginalFirstThunk) / sizeof(IMAGE_THUNK_DATA64)
                : 0;
            for (size_t i = 0; i < limit; ++i)
            {
                if (!iat[i].u1.Function)
                    break;

                bool nameMatches = false;
                if (names && i < nameLimit && names[i].u1.AddressOfData &&
                    !IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) &&
                    inImage(names[i].u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME)))
                {
                    const auto nameRva = names[i].u1.AddressOfData +
                        offsetof(IMAGE_IMPORT_BY_NAME, Name);
                    if (inImage(nameRva))
                    {
                        const char* importedName = reinterpret_cast<const char*>(base + nameRva);
                        if (std::memchr(importedName, '\0', imageSize - nameRva))
                            nameMatches = strcmp(importedName, functionName) == 0;
                    }
                }

                auto* slot = reinterpret_cast<void**>(&iat[i].u1.Function);
                const void* current = *slot;
                if (!nameMatches && current != expectedAddress)
                    continue;
                if (current == replacement)
                {
                    detail = "IAT hook already installed";
                    return true;
                }

                DWORD oldProtect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
                {
                    detail = "VirtualProtect(IAT) failed: " + std::to_string(GetLastError());
                    return false;
                }

                void* previous = InterlockedExchangePointer(
                    reinterpret_cast<void* volatile*>(slot), replacement);
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                *originalAddress = previous;
                detail = "hooked MFCreateSinkWriterFromURL through executable IAT";
                return true;
            }
            detail = "MFCreateSinkWriterFromURL absent from executable import table";
            return false;
        }
        detail = "main executable does not import mfreadwrite.dll";
        return false;
    }
}
