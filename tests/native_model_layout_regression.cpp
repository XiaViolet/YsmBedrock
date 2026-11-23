// Run against the installed Minecraft.Windows.exe files. Maps resources as read-only data.
#include "NativeModelLayout.h"
#include <array>
#include <iostream>

using namespace ysm::native_model_layout;

bool checkImage(wchar_t const* path) {
    auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return false;
    auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!view) return false;
    auto base = reinterpret_cast<std::uintptr_t>(view);
    auto* dos = static_cast<IMAGE_DOS_HEADER const*>(view);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    bool model = supports(base, base + modelVtable);
    bool renderer = matches(base, rendererConstructor, rendererSignature);
    bool badTypeRejected = !supports(base, base + modelVtable + sizeof(void*));
    struct Header { IMAGE_NT_HEADERS64 nt; std::array<IMAGE_SECTION_HEADER, 2> sections; };
    Header header{*nt, {IMAGE_FIRST_SECTION(nt)[0], IMAGE_FIRST_SECTION(nt)[1]}};
    --header.sections[1].VirtualAddress;
    bool movedDataRejected = !supportedSections(header.nt);
    bool previous = model && nt->OptionalHeader.SizeOfImage == 0x13847000;
    std::cout << "image_bytes=" << nt->OptionalHeader.SizeOfImage << " gate_0.29.41=" << previous
        << " gate_current=" << model << " renderer_signature=" << renderer
        << " wrong_type_rejected=" << badTypeRejected << " moved_rdata_rejected=" << movedDataRejected << '\n';
    UnmapViewOfFile(view);
    return model && renderer && badTypeRejected && movedDataRejected;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    for (int i = 1; i < argc; ++i) if (!checkImage(argv[i])) return 1;
    return 0;
}
