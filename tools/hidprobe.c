/* hidprobe.c - read-only: list HID interfaces of a VID/PID and their report sizes.
   Opens each interface with zero access rights (query only); sends nothing. */
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    USHORT vid = argc > 1 ? (USHORT)strtoul(argv[1], NULL, 16) : 0x1A86;
    USHORT pid = argc > 2 ? (USHORT)strtoul(argv[2], NULL, 16) : 0xFE07;
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = malloc(need);
        d->cbSize = sizeof *d;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, d, need, NULL, NULL)) { free(d); continue; }
        HANDLE h = CreateFileW(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES a = { sizeof a };
            if (HidD_GetAttributes(h, &a) && a.VendorID == vid && a.ProductID == pid) {
                wchar_t man[128] = L"", prod[128] = L"", ser[128] = L"";
                HidD_GetManufacturerString(h, man, sizeof man);
                HidD_GetProductString(h, prod, sizeof prod);
                HidD_GetSerialNumberString(h, ser, sizeof ser);
                PHIDP_PREPARSED_DATA pp; HIDP_CAPS c = {0};
                if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &c); HidD_FreePreparsedData(pp); }
                wprintf(L"%ls\n  VID=%04X PID=%04X rev=%04X  mfr='%ls' product='%ls' serial='%ls'\n"
                        L"  UsagePage=0x%04X Usage=0x%04X  InLen=%u OutLen=%u FeatLen=%u\n",
                        d->DevicePath, a.VendorID, a.ProductID, a.VersionNumber, man, prod, ser,
                        c.UsagePage, c.Usage, c.InputReportByteLength, c.OutputReportByteLength,
                        c.FeatureReportByteLength);
            }
            CloseHandle(h);
        }
        free(d);
    }
    SetupDiDestroyDeviceInfoList(set);
    return 0;
}
