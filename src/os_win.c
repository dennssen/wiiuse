/*
 *	wiiuse
 *
 *	Written By:
 *		Michael Laforest	< para >
 *		Email: < thepara (--AT--) g m a i l [--DOT--] com >
 *
 *	Copyright 2006-2007
 *
 *	This file is part of wiiuse.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *	$Header$
 *
 */

/**
 *	@file
 *	@brief Handles device I/O for Windows.
 */

#include <time.h>

#include "events.h"
#include "io.h"
#include "os.h"

#ifdef WIIUSE_WIN32
#include <stdlib.h>
#include "bluetoothapis.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <hidsdi.h>
#include <setupapi.h>

bool wchar_starts_with(const wchar_t *str, const wchar_t *prefix) {
    if (!str || !prefix) {
        return false;
    }

    return wcsncmp(str, prefix, wcslen(prefix)) == 0;
}

bool is_wiimote_name(WCHAR *name) {
    return wchar_starts_with(name, L"Nintendo RVL-CNT");
}

bool should_remove_wiimote(BLUETOOTH_DEVICE_INFO* btdi) {
    return btdi->fRemembered && ! btdi->fConnected && !btdi->fAuthenticated;
}

bool try_remove_wiimote(HANDLE radio_handle, const BLUETOOTH_RADIO_INFO* radio_info, BLUETOOTH_DEVICE_INFO* btdi) {
    if (!should_remove_wiimote(btdi)) {
        return true;
    }

    printf("Removing Wiimote\n");

    const auto remove_device_result = BluetoothRemoveDevice(&btdi->Address);
    if (remove_device_result != ERROR_SUCCESS) {
        printf("BluetoothRemoveDevice failed: %lu\n", remove_device_result);
        return true;
    }

    return true;
}

bool try_pair_wiimote(HANDLE radio_handle, const BLUETOOTH_RADIO_INFO* radio_info, BLUETOOTH_DEVICE_INFO* btdi) {
    if (btdi->fConnected)
        return true;

    const auto name = btdi->szName;

    const auto service_state_result = BluetoothSetServiceState(
        radio_handle,
        btdi,
        &HumanInterfaceDeviceServiceClass_UUID,
        BLUETOOTH_SERVICE_ENABLE
    );
    if (service_state_result != ERROR_SUCCESS) {
        printf("BluetoothSetServiceState failed: %lu\n", service_state_result);
        return true;
    }

    return true;
}

bool is_wiimote(HANDLE radio_handle, const BLUETOOTH_RADIO_INFO* radio_info, BLUETOOTH_DEVICE_INFO* btdi) {
    const auto name = btdi->szName;

    return is_wiimote_name(name);
}

bool search_devices(HANDLE radio_handle, BLUETOOTH_DEVICE_SEARCH_PARAMS search_params, bool (*callback)(HANDLE, const BLUETOOTH_RADIO_INFO*, BLUETOOTH_DEVICE_INFO*)) {
    BLUETOOTH_RADIO_INFO radio_info = {};
    radio_info.dwSize = sizeof(radio_info);

    if (BluetoothGetRadioInfo(radio_handle, &radio_info) != ERROR_SUCCESS) {
        return true;
    }

    search_params.hRadio = radio_handle;

    BLUETOOTH_DEVICE_INFO btdi = {};
    btdi.dwSize = sizeof(btdi);

    const auto find_device = BluetoothFindFirstDevice(&search_params, &btdi);
    if (find_device == nullptr) {
        return true;
    }

    while (true) {
        if (!is_wiimote(radio_handle, &radio_info, &btdi))
            break;

        if (!callback(radio_handle, &radio_info, &btdi))
            break;

        if (!BluetoothFindNextDevice(find_device, &btdi))
            break;
    }

    BluetoothFindDeviceClose(find_device);
    return true;
}

void search_radios(BLUETOOTH_DEVICE_SEARCH_PARAMS search_params, bool (*callback)(HANDLE, const BLUETOOTH_RADIO_INFO*, BLUETOOTH_DEVICE_INFO*)) {
    BLUETOOTH_FIND_RADIO_PARAMS radio_params = {};
    radio_params.dwSize = sizeof(radio_params);

    HANDLE radio_handle = {};
    const auto find_radio = BluetoothFindFirstRadio(&radio_params, &radio_handle);
    if (find_radio == nullptr) {
        printf("Failed to find radio\n");
        return;
    }

    while (true) {
        const auto control = search_devices(radio_handle, search_params, callback);
        CloseHandle(radio_handle);

        if (!control)
            break;

        if (!BluetoothFindNextRadio(find_radio, &radio_handle))
            break;
    }

    BluetoothFindRadioClose(find_radio);
}

void enumerate_bluetooth_wiimotes(uint8_t inquiry_length, bool (*callback)(HANDLE, const BLUETOOTH_RADIO_INFO*, BLUETOOTH_DEVICE_INFO*)) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search_params = {
        .fReturnAuthenticated = true,
        .fReturnRemembered = true,
        .fReturnUnknown = true,
        .fReturnConnected = true,
        .fIssueInquiry = inquiry_length > 0,
        .cTimeoutMultiplier = inquiry_length
    };
    search_params.dwSize = sizeof(search_params);

    search_radios(search_params, callback);
}

void discover_and_pair_wiimotes(uint8_t inquiry_length) {
    enumerate_bluetooth_wiimotes(inquiry_length, try_pair_wiimote);
}

void remove_remembered_wiimotes() {
    enumerate_bluetooth_wiimotes(0, try_remove_wiimote);
}

unsigned long wiiuse_os_ticks()
{
    FILETIME ft;
    ULARGE_INTEGER hnsTime;

#if _WIN32_WINNT >= _WIN32_WINNT_WIN10
    GetSystemTimePreciseAsFileTime(&ft);
#else
    GetSystemTimeAsFileTime(&ft);
#endif

    hnsTime.LowPart = ft.dwLowDateTime;
    hnsTime.HighPart = ft.dwHighDateTime;

    return hnsTime.QuadPart / 10000ULL;
}

int wiiuse_os_find(struct wiimote_t **wm, int max_wiimotes, int timeout)
{
    GUID device_id;
    HANDLE dev;
    HDEVINFO device_info;
    int i, index;
    DWORD len;
    SP_DEVICE_INTERFACE_DATA device_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data = NULL;
    HIDD_ATTRIBUTES attr;
    int found = 0;

    (void)timeout; /* unused */

    device_data.cbSize = sizeof(device_data);
    index              = 0;

    remove_remembered_wiimotes();
    discover_and_pair_wiimotes(3);

    /* get the device id */
    HidD_GetHidGuid(&device_id);

    /* get all hid devices connected */
    device_info = SetupDiGetClassDevs(&device_id, NULL, NULL, (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));

    for (;; ++index)
    {

        if (detail_data)
        {
            free(detail_data);
            detail_data = NULL;
        }

        /* query the next hid device info */
        if (!SetupDiEnumDeviceInterfaces(device_info, NULL, &device_id, index, &device_data))
        {
            break;
        }

        /* get the size of the data block required */
        i                   = SetupDiGetDeviceInterfaceDetail(device_info, &device_data, NULL, 0, &len, NULL);
        detail_data         = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(len);
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        /* query the data for this device */
        if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_data, detail_data, len, NULL, NULL))
        {
            continue;
        }

        /* open the device */
        dev = CreateFile(detail_data->DevicePath, (GENERIC_READ | GENERIC_WRITE),
                         (FILE_SHARE_READ | FILE_SHARE_WRITE), NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         NULL);
        if (dev == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        /* get device attributes */
        attr.Size = sizeof(attr);
        i         = HidD_GetAttributes(dev, &attr);

        if ((attr.VendorID == WM_VENDOR_ID)
            && ((attr.ProductID == WM_PRODUCT_ID) || (attr.ProductID == WM_PRODUCT_ID_TR)))
        {
            /* this is a wiimote */
            wm[found]->dev_handle = dev;

            if (attr.ProductID == WM_PRODUCT_ID_TR)
                wm[found]->type = WIIUSE_WIIMOTE_MOTION_PLUS_INSIDE;

            wm[found]->hid_overlap.hEvent     = CreateEvent(NULL, 1, 1, "");
            wm[found]->hid_overlap.Offset     = 0;
            wm[found]->hid_overlap.OffsetHigh = 0;

            WIIMOTE_ENABLE_STATE(wm[found], WIIMOTE_STATE_DEV_FOUND);
            WIIMOTE_ENABLE_STATE(wm[found], WIIMOTE_STATE_CONNECTED);

            /* try to set the output report to see if the device is actually connected */
            if (!wiiuse_set_report_type(wm[found]))
            {
                WIIMOTE_DISABLE_STATE(wm[found], WIIMOTE_STATE_CONNECTED);
                continue;
            }

            /* do the handshake */
            wiiuse_handshake(wm[found], NULL, 0);

            WIIUSE_INFO("Connected to wiimote [id %i].", wm[found]->unid);

            ++found;
            if (found >= max_wiimotes)
            {
                break;
            }
        } else
        {
            /* not a wiimote */
            CloseHandle(dev);
        }
    }

    if (detail_data)
    {
        free(detail_data);
    }

    SetupDiDestroyDeviceInfoList(device_info);

    return found;
}

int wiiuse_os_connect(struct wiimote_t **wm, int wiimotes)
{
    int connected = 0;
    int i         = 0;

    for (; i < wiimotes; ++i)
    {
        if (!wm[i])
        {
            continue;
        }
        if (WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_CONNECTED))
        {
            ++connected;
        }
    }

    return connected;
}

void wiiuse_os_disconnect(struct wiimote_t *wm)
{
    if (!wm || WIIMOTE_IS_CONNECTED(wm))
    {
        return;
    }

    CloseHandle(wm->dev_handle);
    wm->dev_handle = 0;

    ResetEvent(&wm->hid_overlap);

    wm->event = WIIUSE_NONE;

    WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_CONNECTED);
    WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_HANDSHAKE);
}

int wiiuse_os_poll(struct wiimote_t **wm, int wiimotes)
{
    int i;
    byte read_buffer[MAX_PAYLOAD];
    int evnt = 0;

    if (!wm)
    {
        return 0;
    }

    for (i = 0; i < wiimotes; ++i)
    {
        wm[i]->event = WIIUSE_NONE;

        /* clear out the buffer */
        memset(read_buffer, 0, sizeof(read_buffer));
        /* read */
        if (wiiuse_os_read(wm[i], read_buffer, sizeof(read_buffer)))
        {
            /* propagate the event */
            propagate_event(wm[i], read_buffer[0], read_buffer + 1);
            evnt += (wm[i]->event != WIIUSE_NONE);
        } else
        {
            /* send out any waiting writes */
            wiiuse_send_next_pending_write_request(wm[i]);
            idle_cycle(wm[i]);
        }
    }

    return evnt;
}

int wiiuse_os_read(struct wiimote_t *wm, byte *buf, int len)
{
    DWORD b, r;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm))
    {
        return 0;
    }

    if (!ReadFile(wm->dev_handle, buf, len, &b, &wm->hid_overlap))
    {
        /* partial read */
        b = GetLastError();

        if ((b == ERROR_HANDLE_EOF) || (b == ERROR_DEVICE_NOT_CONNECTED))
        {
            /* remote disconnect */
            wiiuse_disconnected(wm);
            return 0;
        }

        r = WaitForSingleObject(wm->hid_overlap.hEvent, wm->timeout);
        if (r == WAIT_TIMEOUT)
        {
            /* timeout - cancel and continue */

            /*
                        if (*buf) {
                                WIIUSE_WARNING("Packet ignored.  This may indicate a problem (timeout is %i
               ms).", wm->timeout);
                        }
            */

            CancelIo(wm->dev_handle);
            ResetEvent(wm->hid_overlap.hEvent);
            return 0;
        } else if (r == WAIT_FAILED)
        {
            WIIUSE_WARNING("A wait error occurred on reading from wiimote %i.", wm->unid);
            return 0;
        }

        if (!GetOverlappedResult(wm->dev_handle, &wm->hid_overlap, &b, 0))
        {
            return 0;
        }

/* log the received data */
#ifdef WITH_WIIUSE_DEBUG
        {
            DWORD i;
            printf("[DEBUG] (id %i) RECV: (%.2x) ", wm->unid, buf[0]);
            for (i = 1; i < b; i++)
            {
                printf("%.2x ", buf[i]);
            }
            printf("\n");
        }
#endif
    }

    ResetEvent(wm->hid_overlap.hEvent);
    return 1;
}

int wiiuse_os_write(struct wiimote_t *wm, byte report_type, byte *buf, int len)
{
    DWORD bytes;
    int i;
    byte write_buffer[MAX_PAYLOAD];

    if (!wm || !WIIMOTE_IS_CONNECTED(wm))
    {
        return 0;
    }

    write_buffer[0] = report_type;
    memcpy(write_buffer + 1, buf, len);

    switch (wm->stack)
    {
    case WIIUSE_STACK_UNKNOWN:
    {
        /* try to auto-detect the stack type */
        if (i = WriteFile(wm->dev_handle, write_buffer, 22, &bytes, &wm->hid_overlap))
        {
            /* bluesoleil will always return 1 here, even if it's not connected */
            wm->stack = WIIUSE_STACK_BLUESOLEIL;
            return i;
        }

        if (i = HidD_SetOutputReport(wm->dev_handle, write_buffer, len + 1))
        {
            wm->stack = WIIUSE_STACK_MS;
            return i;
        }

        WIIUSE_ERROR("Unable to determine bluetooth stack type.");
        return 0;
    }

  /* Windows should always use WriteFile instead of HidD_SetOutputReport to communicate -> data pipe instead of control pipe*/
    case WIIUSE_STACK_MS:
        return WriteFile(wm->dev_handle, write_buffer, 22, &bytes, &wm->hid_overlap);

    case WIIUSE_STACK_BLUESOLEIL:
        return WriteFile(wm->dev_handle, write_buffer, 22, &bytes, &wm->hid_overlap);
    }

    return 0;
}

void wiiuse_init_platform_fields(struct wiimote_t *wm)
{
    wm->dev_handle     = 0;
    wm->stack          = WIIUSE_STACK_UNKNOWN;
    wm->normal_timeout = WIIMOTE_DEFAULT_TIMEOUT;
    wm->exp_timeout    = WIIMOTE_EXP_TIMEOUT;
    wm->timeout        = wm->normal_timeout;
}

void wiiuse_cleanup_platform_fields(struct wiimote_t *wm) { wm->dev_handle = 0; }

#endif /* ifdef WIIUSE_WIN32 */
