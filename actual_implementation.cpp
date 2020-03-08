#include "framework.h"
#include "tv.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <strsafe.h>
#include <dwmapi.h>
#include <winusb.h>
#include <usb.h>
#include <cfgmgr32.h>

#include <initguid.h>

volatile int best_idx;


// in framework.h
//#define IMG_WIDTH 858
//#define IMG_HEIGHT 263
#define NUM_DISPLAY_FRAMES 8

#define NUM_XFERS 8
#define SMALL_XFER_IN_MS 10

#define CHKERR(Succeeded)                   \
    if (!Succeeded) {                       \
        WCHAR LineNo[16] = {0};             \
        wsprintf(LineNo, L"%d", __LINE__);  \
        ErrorExit(LineNo);                  \
    }

TRACELOGGING_DEFINE_PROVIDER(
    hTrace,
    "Tv",
    (0xc14e8d1, 0x51d7, 0x4460, 0xb3, 0x96, 0x2, 0xeb, 0x6f, 0xfe, 0x8e, 0xf5));

volatile ULONG FrameProduced; /* volatile as it's set by the framing thread and read by the display thread */
ULONG FrameConsumed;
HWND hMainWnd;

RGBQUAD* TempFrame;
RGBQUAD* DisplayFrames[NUM_DISPLAY_FRAMES];

PUCHAR bufWorking;
HANDLE bufWorkingReady;
int bufWorkingBytes;
LONG bufDone;

RGBQUAD* bufDemodWorking;
RGBQUAD* bufDemod;
HANDLE bufDemodReady;
int bufDemodBytes;

int pxPerSkip = 23400;
bool skipPixels = false;
bool skipLine = false;
bool lineDirection = false;

int cpxPerSkip = 23400;
bool cskipPixels = false;
bool cskipLine = false;
bool clineDirection = false;

void compute_sin_table();
void compute_cos_table();

DWORD WINAPI usb_reading_thread(_In_ LPVOID lpParameter);
DWORD WINAPI demodulate2_thread(_In_ LPVOID lpParameter);
DWORD WINAPI framing_thread(_In_ LPVOID lpParameter);
DWORD WINAPI framing_thread2(_In_ LPVOID lpParameter);
DWORD WINAPI framing_thread3(_In_ LPVOID lpParameter);
DWORD WINAPI framing_thread3(_In_ LPVOID lpParameter);
DWORD WINAPI framing_thread4(_In_ LPVOID lpParameter);
DWORD WINAPI dwm_waiter_thread(_In_ LPVOID lpParameter);

DEFINE_GUID(GUID_DEVINTERFACE_USBApplication2,
	0x1ab4ce39, 0x4959, 0x4c97, 0x84, 0x02, 0x85, 0xa2, 0xe5, 0xa2, 0x6b, 0x8f);

typedef struct _DEVICE_DATA {

	BOOL                    HandlesOpen;
	WINUSB_INTERFACE_HANDLE WinusbHandle;
	HANDLE                  DeviceHandle;
	TCHAR                   DevicePath[MAX_PATH];

} DEVICE_DATA, * PDEVICE_DATA;

HRESULT
OpenDevice(
	_Out_     PDEVICE_DATA DeviceData,
	_Out_opt_ PBOOL        FailureDeviceNotFound
);

VOID
CloseDevice(
	_Inout_ PDEVICE_DATA DeviceData
);

void ErrorExit(LPTSTR lpszFunction);

LRESULT TvWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static BITMAPINFO bi;
	static UINT_PTR timer;
	static ULONG frame;
	static ULONG64 lastPresent;
	static ULONG64 skipCnt = 0;
	static ULONG64 lastTimer = 0;
	static int rateControlMult = -140;
	//ULONG64 nowTimer;
	switch (message)
	{
	case WM_CREATE:
		hMainWnd = hWnd;
		bi.bmiHeader.biSize = sizeof(bi);
		bi.bmiHeader.biWidth = IMG_WIDTH;
		bi.bmiHeader.biHeight = -1 * IMG_HEIGHT * 2;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		bi.bmiHeader.biSizeImage = 0;

		TempFrame = (RGBQUAD*)malloc(IMG_WIDTH * IMG_HEIGHT * sizeof(RGBQUAD));
		for (int i = 0; i < NUM_DISPLAY_FRAMES; i++) {
			DisplayFrames[i] = (RGBQUAD*)malloc(IMG_WIDTH * IMG_HEIGHT * 2 * sizeof(RGBQUAD));
			ZeroMemory(DisplayFrames[i], IMG_WIDTH * IMG_HEIGHT * 2 * sizeof(RGBQUAD));
		}

		AllocConsole();
		freopen("CONOUT$", "w", stdout);

		bufWorkingReady = CreateEvent(NULL, FALSE, FALSE, NULL);
        bufDemodReady = CreateEvent(NULL, FALSE, FALSE, NULL);
		FrameConsumed = 0;
        compute_cos_table();
        compute_sin_table();
		CreateThread(NULL, 0, usb_reading_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, dwm_waiter_thread, NULL, 0, NULL);
        //CreateThread(NULL, 0, demodulate2_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, framing_thread4, NULL, 0, NULL);
		break;

	case WM_PAINT:
	{
		TraceLoggingWrite(hTrace, "PaintBegin");

		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);

        // This FIFO deals with crossing the different clock domains of the incoming video signal versus the display controller.
        // The incoming video signal is going to be ~59.94 fields per second, while the display is refresihng at 60 fps. Eventually
        // there will be enough of a mismatch that we'l need to duplicate a frame so that the incoming signal can catch up.
		auto p = FrameProduced;
		auto c = FrameConsumed;

		if (p > c) {
			// The FIFO has data.

			if (p > (c + 7)) {
				// More frames produced than consumed. This should never happen, but if for some reason we aren't given CPU
                // time for ages, it could. Empty even more frames. There'll be a glitch, but oh well.
				TraceLoggingWrite(hTrace, "Overrun", TraceLoggingValue(p, "Produced"), TraceLoggingValue(c, "Consumed"));
				printf("Overrun\n");
                FrameConsumed += 4;
				//DebugBreak();
			}
			else if (p == (c + 7)) {
				// The FIFO is full. It should never get completely full as we should have been skipping frames up until now.
				// Skip a bunch of frames to empty it quickly.
				FrameConsumed += 2;
				TraceLoggingWrite(hTrace, "Full", TraceLoggingValue(p, "Produced"), TraceLoggingValue(c, "Consumed"));
			}
			else if (p > (c + 5)) {
				// The FIFO is almost full. Skip a frame to prevent it completely filling up;
				FrameConsumed++;
				TraceLoggingWrite(hTrace, "AlmostFull", TraceLoggingValue(p, "Produced"), TraceLoggingValue(c, "Consumed"));
			}
		}
		else if (p == c) {
			// The FIFO is empty. Repeat the last frame drawn.
			TraceLoggingWrite(hTrace, "Empty", TraceLoggingValue(p, "Produced"), TraceLoggingValue(c, "Consumed"));
			FrameConsumed--;
		}
		else {
			// More frames were consumed than produced; this should never happen. Same as Overrun, this shouldn't ever
            // happen, but also could given scheduling irregularities. If the USB reading and framing threads were
            // starved long enough for this to happen tho, it's more likely they'll ErrorExit when the device dies.
			TraceLoggingWrite(hTrace, "Underrun", TraceLoggingValue(p, "Produced"), TraceLoggingValue(c, "Consumed"));
			printf("Underrun\n");
			DebugBreak();
		}

		TraceLoggingWrite(hTrace,
			"Draw",
			TraceLoggingValue(p, "Produced"),
			TraceLoggingValue(c, "Consumed"));

		SetDIBitsToDevice(
			hdc,
			0, 0, IMG_WIDTH, IMG_HEIGHT * 2, 0, 0, 0, IMG_HEIGHT * 2,
			DisplayFrames[FrameConsumed % NUM_DISPLAY_FRAMES],
			&bi,
			DIB_RGB_COLORS);

		FrameConsumed++;

		EndPaint(hWnd, &ps);
		TraceLoggingWrite(hTrace, "PaintEnd");

	}
	break;
	case WM_KEYDOWN:
		switch (wParam)
		{
		case VK_LEFT:
			lineDirection = true;
			skipLine = true;

			break;

		case VK_RIGHT:
			lineDirection = false;
			skipLine = true;

			break;

		case VK_UP:
			pxPerSkip += 100;
			printf("%d\n", pxPerSkip);
			break;

		case VK_DOWN:
			pxPerSkip -= 100;
			printf("%d\n", pxPerSkip);
			break;
		case 0x32:
			best_idx++;
			printf("best_idx now %d\n", best_idx);
			break;
		case 0x31:
			best_idx--;
			printf("best_idx now %d\n", best_idx);
			break;
        case 0x33:
            cpxPerSkip += 100;
            printf("%d\n", cpxPerSkip);
            break;
        case 0x34:
            cpxPerSkip -= 100;
            printf("%d\n", cpxPerSkip);
            break;
        case 0x35:
            cpxPerSkip += 1;
            printf("%d\n", cpxPerSkip);
            break;
        case 0x36:
            cpxPerSkip -= 1;
            printf("%d\n", cpxPerSkip);
            break;
        }
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}


HRESULT
RetrieveDevicePath(
    _Out_bytecap_(BufLen) LPTSTR DevicePath,
    _In_                  ULONG  BufLen,
    _Out_opt_             PBOOL  FailureDeviceNotFound
);

HRESULT
OpenDevice(
    _Out_     PDEVICE_DATA DeviceData,
    _Out_opt_ PBOOL        FailureDeviceNotFound
)
/*++

Routine description:

    Open all needed handles to interact with the device.

    If the device has multiple USB interfaces, this function grants access to
    only the first interface.

    If multiple devices have the same device interface GUID, there is no
    guarantee of which one will be returned.

Arguments:

    DeviceData - Struct filled in by this function. The caller should use the
        WinusbHandle to interact with the device, and must pass the struct to
        CloseDevice when finished.

    FailureDeviceNotFound - TRUE when failure is returned due to no devices
        found with the correct device interface (device not connected, driver
        not installed, or device is disabled in Device Manager); FALSE
        otherwise.

Return value:

    HRESULT

--*/
{
    HRESULT hr = S_OK;
    BOOL    bResult;

    DeviceData->HandlesOpen = FALSE;

    hr = RetrieveDevicePath(DeviceData->DevicePath,
        sizeof(DeviceData->DevicePath),
        FailureDeviceNotFound);

    if (FAILED(hr)) {

        return hr;
    }

    DeviceData->DeviceHandle = CreateFile(DeviceData->DevicePath,
        GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (INVALID_HANDLE_VALUE == DeviceData->DeviceHandle) {

        hr = HRESULT_FROM_WIN32(GetLastError());
        return hr;
    }

    bResult = WinUsb_Initialize(DeviceData->DeviceHandle,
        &DeviceData->WinusbHandle);

    if (FALSE == bResult) {

        hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(DeviceData->DeviceHandle);
        return hr;
    }

    DeviceData->HandlesOpen = TRUE;
    return hr;
}

VOID
CloseDevice(
    _Inout_ PDEVICE_DATA DeviceData
)
/*++

Routine description:

    Perform required cleanup when the device is no longer needed.

    If OpenDevice failed, do nothing.

Arguments:

    DeviceData - Struct filled in by OpenDevice

Return value:

    None

--*/
{
    if (FALSE == DeviceData->HandlesOpen) {

        //
        // Called on an uninitialized DeviceData
        //
        return;
    }

    WinUsb_Free(DeviceData->WinusbHandle);
    CloseHandle(DeviceData->DeviceHandle);
    DeviceData->HandlesOpen = FALSE;

    return;
}

HRESULT
RetrieveDevicePath(
    _Out_bytecap_(BufLen) LPTSTR DevicePath,
    _In_                  ULONG  BufLen,
    _Out_opt_             PBOOL  FailureDeviceNotFound
)
/*++

Routine description:

    Retrieve the device path that can be used to open the WinUSB-based device.

    If multiple devices have the same device interface GUID, there is no
    guarantee of which one will be returned.

Arguments:

    DevicePath - On successful return, the path of the device (use with CreateFile).

    BufLen - The size of DevicePath's buffer, in bytes

    FailureDeviceNotFound - TRUE when failure is returned due to no devices
        found with the correct device interface (device not connected, driver
        not installed, or device is disabled in Device Manager); FALSE
        otherwise.

Return value:

    HRESULT

--*/
{
    CONFIGRET cr = CR_SUCCESS;
    HRESULT   hr = S_OK;
    PTSTR     DeviceInterfaceList = NULL;
    ULONG     DeviceInterfaceListLength = 0;

    if (NULL != FailureDeviceNotFound) {

        *FailureDeviceNotFound = FALSE;
    }

    //
    // Enumerate all devices exposing the interface. Do this in a loop
    // in case a new interface is discovered while this code is executing,
    // causing CM_Get_Device_Interface_List to return CR_BUFFER_SMALL.
    //
    do {
        cr = CM_Get_Device_Interface_List_Size(&DeviceInterfaceListLength,
            (LPGUID)& GUID_DEVINTERFACE_USBApplication2,
            NULL,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

        if (cr != CR_SUCCESS) {
            hr = HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_INVALID_DATA));
            break;
        }

        DeviceInterfaceList = (PTSTR)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            DeviceInterfaceListLength * sizeof(TCHAR));

        if (DeviceInterfaceList == NULL) {
            hr = E_OUTOFMEMORY;
            break;
        }

        cr = CM_Get_Device_Interface_List((LPGUID)& GUID_DEVINTERFACE_USBApplication2,
            NULL,
            DeviceInterfaceList,
            DeviceInterfaceListLength,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

        if (cr != CR_SUCCESS) {
            HeapFree(GetProcessHeap(), 0, DeviceInterfaceList);

            if (cr != CR_BUFFER_SMALL) {
                hr = HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_INVALID_DATA));
            }
        }
    } while (cr == CR_BUFFER_SMALL);

    if (FAILED(hr)) {
        return hr;
    }

    //
    // If the interface list is empty, no devices were found.
    //
    if (*DeviceInterfaceList == TEXT('\0')) {
        if (NULL != FailureDeviceNotFound) {
            *FailureDeviceNotFound = TRUE;
        }

        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        HeapFree(GetProcessHeap(), 0, DeviceInterfaceList);
        return hr;
    }

    //
    // Give path of the first found device interface instance to the caller. CM_Get_Device_Interface_List ensured
    // the instance is NULL-terminated.
    //
    hr = StringCbCopy(DevicePath,
        BufLen,
        DeviceInterfaceList);

    HeapFree(GetProcessHeap(), 0, DeviceInterfaceList);

    return hr;
}

DWORD WINAPI usb_reading_thread(_In_ LPVOID lpParameter)
{
	DEVICE_DATA           deviceData;
	HRESULT               hr;
	USB_DEVICE_DESCRIPTOR deviceDesc;
	BOOL                  bResult;
	BOOL                  noDevice;
	ULONG                 lengthReceived;
    PUCHAR                bufIn;

	// Use with debugger to allow it time to initialize before time-sensitive USB operations occur.
	Sleep(2000);

	BOOL Succeeded = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	if (!Succeeded) DebugBreak();

	//
	// Find a device connected to the system that has WinUSB installed using our
	// INF
	//

	hr = OpenDevice(&deviceData, &noDevice);
	if (FAILED(hr)) {
		if (noDevice) {
			wprintf(L"Device not connected or driver not installed\n");
		}
		else {
			wprintf(L"Failed looking for device, HRESULT 0x%x\n", hr);
		}

		return 0;
	}

	//
	// Get device descriptor
	//
	bResult = WinUsb_GetDescriptor(deviceData.WinusbHandle,
		USB_DEVICE_DESCRIPTOR_TYPE,
		0,
		0,
		(PBYTE)& deviceDesc,
		sizeof(deviceDesc),
		&lengthReceived);

	if (FALSE == bResult || lengthReceived != sizeof(deviceDesc)) {
		wprintf(L"Error among LastError %d or lengthReceived %d\n",
			FALSE == bResult ? GetLastError() : 0,
			lengthReceived);
		CloseDevice(&deviceData);
		return 0;
	}

	//
	// Print a few parts of the device descriptor
	//
	wprintf(L"Device found: VID_%04X&PID_%04X; bcdUsb %04X\n",
		deviceDesc.idVendor,
		deviceDesc.idProduct,
		deviceDesc.bcdUSB);

	USB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor;
	Succeeded = WinUsb_QueryInterfaceSettings(deviceData.WinusbHandle, 1, &UsbAltInterfaceDescriptor);
	CHKERR(Succeeded);

	Succeeded = WinUsb_SetCurrentAlternateSetting(deviceData.WinusbHandle, UsbAltInterfaceDescriptor.bAlternateSetting);
	CHKERR(Succeeded);

	//
	// Get information about the pipe at alternate setting 1, which should be a isochronous input pipe with 1024*3 byte transfers.
	//

	WINUSB_PIPE_INFORMATION pi;
	Succeeded = WinUsb_QueryPipe(deviceData.WinusbHandle, 1, 0, &pi);
	CHKERR(Succeeded);
	WINUSB_PIPE_INFORMATION_EX pix;
	Succeeded = WinUsb_QueryPipeEx(deviceData.WinusbHandle, 1, 0, &pix);
	CHKERR(Succeeded);

	// Because this is high speed, there's 8 microframes per interval, so when it says MaximumBytesPerInterval, there can be 8 of those
	// per interval. Confusingly, the Interval value tells you frequently the device is actually going to give you data. It could be every
	// microframe, or it could be every other microframe, or every 4th, or every 8th.
	// So this says, we have 3072 bytes per interval, and the device says that it's every interval, so that's automatically
	// 24576 bytes per frame in the worst case. Since this transfers per frame, we need to allocate at least that much space in the buffer.

	ULONG TransferSize = SMALL_XFER_IN_MS * pix.MaximumBytesPerInterval * (8 / pix.Interval);

	//
	// Allocate buffers to hold the data received from the USB device, and register them with WinUSB.
    // (This effectively pins the buffers in memory so they can't be swapped out the memory manager and so incoming USB data can be DMAed into them.)
    //
	// Create events for the I/O asynchronous completions.
    //
	// Allocate the packet descriptors for when the I/O completes.
    // (These packet descriptors could each use a seperate registered buffer I suppose, but here I just allocated a buffer big enough to hold the
    //  data for all of them, and then each descriptor points to a different location in the big registered buffer.)
	//

	PUCHAR data;
	WINUSB_ISOCH_BUFFER_HANDLE handle;
	OVERLAPPED completions[NUM_XFERS];

	data = (PUCHAR)malloc(NUM_XFERS * (size_t)TransferSize);
	Succeeded = WinUsb_RegisterIsochBuffer(deviceData.WinusbHandle, pi.PipeId, data, NUM_XFERS * TransferSize, &handle);
	CHKERR(Succeeded);

	for (int i = 0; i < NUM_XFERS; i++) {
		ZeroMemory(&completions[i], sizeof(completions[i]));
		completions[i].hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	}

	ULONG PacketCount = (NUM_XFERS * TransferSize) / pix.MaximumBytesPerInterval;
	PUSBD_ISO_PACKET_DESCRIPTOR descr = (PUSBD_ISO_PACKET_DESCRIPTOR)malloc(sizeof(USBD_ISO_PACKET_DESCRIPTOR) * PacketCount);
	for (ULONG i = 0; i < PacketCount; i++) {
		ZeroMemory(&descr[i], sizeof(USBD_ISO_PACKET_DESCRIPTOR));
	}

	// Allocate an extra buffer for the consuming thread to use. We'll memcpy the data into this when it's ready.
    bufWorkingBytes = NUM_XFERS * TransferSize;
    bufWorking = (PUCHAR)malloc(NUM_XFERS * TransferSize);
    bufDemod = (RGBQUAD*)malloc(NUM_XFERS * TransferSize * sizeof(RGBQUAD));
    bufDemodWorking = (RGBQUAD*)malloc(NUM_XFERS * TransferSize * sizeof(RGBQUAD));
    bufIn = (PUCHAR)malloc(NUM_XFERS * TransferSize);
	printf("Buffer size is %d bytes\n", bufWorkingBytes);

	bool primed = false;
	bool firstio = true;

    // Queue all of the read requests with the WinUsb framework. The framework will automatically handle the USB sequence numbers if every request
    // except for the first one has ContinueStream true (which is the reason for the !firstio there).
	for (int i = 0; i < NUM_XFERS; i++) {
		Succeeded = WinUsb_ReadIsochPipeAsap(handle, i * TransferSize, TransferSize, !firstio, PacketCount / NUM_XFERS, &descr[(PacketCount / NUM_XFERS) * i], &completions[i]);
		if (!Succeeded) {
			if (GetLastError() != ERROR_IO_PENDING) {
				CHKERR(Succeeded);
			}
		}

		firstio = false;
	}

    // Now, run the infinite loop, waiting for each request in sequence to complete, copying its data out into a sequential buffer, then re-queuing
    // the request back to the framework. The idea here is to always have enough requests queued so that the WinUsb framework always has a few ready
    // to keep streaming USB data into from the peripheral.
    // Unlike before, all of these requests are queued with ContinueStream == true because we want to keep streaming with no break in sequence numbers.
	int underrunUntilBreak = 10;
	while (true) {
		SIZE_T bufInBytes = 0;
		size_t offsetInDstBuffer = 0;
		for (int i = 0; i < NUM_XFERS; i++) {
			ULONG NumBytes;
			Succeeded = WinUsb_GetOverlappedResult(deviceData.WinusbHandle, &completions[i], &NumBytes, TRUE);
			CHKERR(Succeeded);
			TraceLoggingWrite(hTrace, "TransferProcess", TraceLoggingValue(i, "Number"));

			int startDescriptor = (PacketCount / NUM_XFERS) * i;
			int endDescriptor = startDescriptor + PacketCount / NUM_XFERS;

			size_t bytesReceivedThisTransfer = 0;

			ULONG smallestPacketReceived = ULONG_MAX;
			ULONG largestPacketReceived = 0;

			if (i == 0) {
				TraceLoggingWrite(hTrace, "TransferBegin");
				if (bufDone == 0) {
					TraceLoggingWrite(hTrace, "BufferUnderrun");
					underrunUntilBreak--;
					if (!underrunUntilBreak) {
						//DebugBreak();
					}
				}
				bufDone = 0;
			}

			for (int j = startDescriptor; j < endDescriptor; j++) {

				size_t offsetInSrcBuffer = (i * TransferSize) + descr[j].Offset;
				memcpy(bufIn + offsetInDstBuffer, data + offsetInSrcBuffer, descr[j].Length);

				offsetInDstBuffer += descr[j].Length;
				bytesReceivedThisTransfer += descr[j].Length;

				TraceLoggingWrite(
					hTrace,
					"Transfer",
					TraceLoggingValue(i, "Number"),
					TraceLoggingValue(j, "Descriptor"),
					TraceLoggingValue(descr[j].Offset, "Offset"),
					TraceLoggingValue(descr[j].Length, "Length")
				);

				CHKERR(descr[j].Status == ERROR_SUCCESS);
				if (descr[j].Length < smallestPacketReceived) smallestPacketReceived = descr[j].Length;
				if (descr[j].Length > largestPacketReceived) largestPacketReceived = descr[j].Length;
			}

			assert(bytesReceivedThisTransfer > ((PacketCount / NUM_XFERS) * 512));
			assert(smallestPacketReceived <= 3072);
			assert(largestPacketReceived >= 512);

			TraceLoggingWrite(
				hTrace,
				"TransferReIssue",
				TraceLoggingValue(bytesReceivedThisTransfer, "Bytes"),
				TraceLoggingValue(smallestPacketReceived, "Smalled"),
				TraceLoggingValue(largestPacketReceived, "Largest")
			);

			Succeeded = WinUsb_ReadIsochPipeAsap(handle, i * TransferSize, TransferSize, true, PacketCount / NUM_XFERS, &descr[(PacketCount / NUM_XFERS) * i], &completions[i]);
			if (!Succeeded) {
				if (GetLastError() != ERROR_IO_PENDING) {
					CHKERR(Succeeded);
				}
			}
		}

        // TODO: Assert this doesn't overflow.
		bufWorkingBytes = (int)offsetInDstBuffer;
		PUCHAR tmp = bufWorking;
		bufWorking = bufIn;
		bufIn = tmp;
		TraceLoggingWrite(hTrace, "TransferEnd");
		SetEvent(bufWorkingReady);
	}

	CloseDevice(&deviceData);
	return 0;
}

typedef struct _Average_Ctx {
    UCHAR avg[8];
    ULONG avg_idx = 0;

} Average_Ctx;

UCHAR MovingAvg8(Average_Ctx* Ctx, UCHAR Sample) {
	Ctx->avg[Ctx->avg_idx] = Sample;
	Ctx->avg_idx = (Ctx->avg_idx + 1) % 8;
	int a = 0;
	for (int i = 0; i < 8; i++) a += Ctx->avg[i];
	a /= 8;
	return a;
}

typedef struct _Average_Ctx_Flt {
	double avg[8];
	ULONG avg_idx = 0;

} Average_Ctx_Flt;

double MovingAvg8Flt(Average_Ctx_Flt* Ctx, double Sample) {
	Ctx->avg[Ctx->avg_idx] = Sample;
	Ctx->avg_idx = (Ctx->avg_idx + 1) % 8;
	double a = 0;
	for (int i = 0; i < 8; i++) a += Ctx->avg[i];
	a /= 8;
	return a;
}

UCHAR ClampLow;
UCHAR ClampHigh;

void Clamp(UCHAR Sample) {
    static ULONG64 cnt;
    static Average_Ctx ctx;
    UCHAR Avg = MovingAvg8(&ctx, Sample);
    if (Avg > ClampHigh) ClampHigh = Avg;
    if (Avg < ClampLow) ClampLow = Avg;
    if ((cnt % 10000) == 0) {
        ClampHigh--;
        ClampLow++;
    }
    cnt++;

    if ((cnt % 12342567) == 0) {
        int l = ClampLow;
        int h = ClampHigh;
        printf("low: %d high: %d\n", l, h);
    }
}

RGBQUAD HandleSample(UCHAR Sample);


DWORD WINAPI framing_thread3(_In_ LPVOID lpParameter) {
    int framex = 0, framey = 0;
    constexpr auto totalPixelsPerFrame = IMG_HEIGHT * IMG_WIDTH;

    while (true) {
        WaitForSingleObject(bufWorkingReady, INFINITE);
        
        auto buf = bufWorking;
        auto bufBytes = bufWorkingBytes;

        auto producedFrameIdx = FrameProduced % NUM_DISPLAY_FRAMES;
        auto producedFrame = &DisplayFrames[producedFrameIdx];

        //int j = 0;
        for (auto i = 0; i < bufBytes; i++) {
            auto sampleIn = buf[i];
            auto sampleOut = producedFrame[i];

            sampleOut->rgbReserved = sampleIn;
            sampleOut->rgbRed = sampleIn;
            sampleOut->rgbGreen = sampleIn;
            sampleOut->rgbBlue = sampleIn;
#if 0
            if (j > totalPixelsPerFrame) {
                FrameProduced++;
                producedFrameIdx = FrameProduced % NUM_DISPLAY_FRAMES;
                producedFrame = &DisplayFrames[producedFrameIdx];
                j++;
            }
#endif
        }
        bufDone = 1;
    }
}

DWORD WINAPI framing_thread2(_In_ LPVOID lpParameter) {
    int framex = 0, framey = 0;

    while (true) {
        WaitForSingleObject(bufWorkingReady, INFINITE);
        auto buf = bufWorking;
        auto bufBytes = bufWorkingBytes;
        for (auto i = 0; i < bufBytes; i++) {
            auto sample = buf[i];

            if (framex >= IMG_WIDTH) {
                framey++;
                framex = 0;
            }

            if (framey == IMG_HEIGHT) {
                auto producedFrame = FrameProduced % NUM_DISPLAY_FRAMES;

                for (int y = 0; y < IMG_HEIGHT; y++) {
                    memcpy(
                        &DisplayFrames[producedFrame][y * IMG_WIDTH],
                        &TempFrame[y * IMG_WIDTH],
                        IMG_WIDTH * sizeof(RGBQUAD));
                }

                FrameProduced++;
                framey = 0;
            }

            auto frame_pixel = &TempFrame[framey * IMG_WIDTH + framex];
            frame_pixel->rgbRed = sample;
            frame_pixel->rgbGreen = sample;
            frame_pixel->rgbBlue = sample;

            framex++;
        }
        bufDone = 1;
    }
}

DWORD WINAPI framing_thread(_In_ LPVOID lpParameter) {
	int framex = 0, framey = 0;
	bool field = false;
	int pxUntilSkip = 0;
	ULONG64 pxcnt = 0;
	int syncX = 0;

	while (true) {
		WaitForSingleObject(bufDemodReady, INFINITE);
		TraceLoggingWrite(hTrace, "FrameBegin");
		auto buf = bufDemod;
		for (auto i = 0; i < bufDemodBytes; i++) {
			auto sample = buf[i];

			if (pxUntilSkip > 0) pxUntilSkip--;
			if (pxUntilSkip == 0) {
				pxUntilSkip = pxPerSkip;
				if (skipPixels) {
					framex--;
				}
				else {
					framex++;
				}
			}

   //         syncX = 0;
			//if (sample < 30) syncX = framex;

			if (framex >= IMG_WIDTH) {
				if (skipLine) {
					if (lineDirection) {
						framey++;
					}
					else {
						framey--;
					}
					skipLine = false;
				}

                framey++;
                framex = 0;
                //if (sample > 30) {
                //    framex = IMG_WIDTH - 1;
                //}
                //else {
                //    framey++;
                //    framex = 0;
                //}
				//if (sample > 30) {
				//	int delta = syncX - (IMG_WIDTH / 2);
				//	pxPerSkip -= delta/300;
				//}
			}

			if ((field && (framey == (IMG_HEIGHT - 1)) && (framex == (IMG_WIDTH / 2))) ||
				(!field && (framey == IMG_HEIGHT))) {
				int producedFrame = FrameProduced % NUM_DISPLAY_FRAMES;
				ULONG p = FrameProduced;

				for (int y = 0; y < IMG_HEIGHT; y++) {
					memcpy(&DisplayFrames[FrameProduced % NUM_DISPLAY_FRAMES]

						[(y * 2 + field) * IMG_WIDTH],

						&TempFrame[y * IMG_WIDTH],
						IMG_WIDTH * sizeof(RGBQUAD));

					memcpy(&DisplayFrames[(FrameProduced + 1) % NUM_DISPLAY_FRAMES]

						[(y * 2 + field) * IMG_WIDTH],

						&TempFrame[y * IMG_WIDTH],
						IMG_WIDTH * sizeof(RGBQUAD));
				}

				ZeroMemory(TempFrame, IMG_HEIGHT * IMG_WIDTH * sizeof(RGBQUAD));

				TraceLoggingWrite(hTrace, "Produce", TraceLoggingValue(p, "Number"), TraceLoggingValue(producedFrame, "ModNumber"));
				FrameProduced++;
				framey = 0;
				field = !field;
			}

            //Clamp(sample);
            //sample -= ClampLow + 50;
            //sample = (double)sample * (256.0 / (ClampHigh - ClampLow - 30));

            //sample = HandleSample(sample);

			auto frame_pixel = &TempFrame[framey * IMG_WIDTH + framex];
   //         
   //         if (syncX) {
   //             frame_pixel->rgbRed = 255;
   //             frame_pixel->rgbGreen = 0;
   //             frame_pixel->rgbBlue = 0;
   //         }
   //         else {
                //frame_pixel->rgbRed = sample;
                //frame_pixel->rgbGreen = sample;
                //frame_pixel->rgbBlue = sample;
   //         }

            *frame_pixel = sample;

			framex++;
			pxcnt++;
		}
		TraceLoggingWrite(hTrace, "FrameEnd");
		bufDone = 1;
	}
}

DWORD WINAPI framing_thread4(_In_ LPVOID lpParameter) {
    int framex = 0, framey = 0;
    bool fieldAlternator = false;
    int pxUntilSkip = 0;
    ULONG64 pxcnt = 0;
    int syncX = 0;
    ULONG64 previousSync = 0;
    int syncDurations[8];
    ULONG syncDurationsIdx = 0;

    while (true) {
        //
        // Wait for a buffer of data to be populated by the USB receiving thread
        //

        WaitForSingleObject(bufWorkingReady, INFINITE);
        TraceLoggingWrite(hTrace, "FrameBegin");
        
        //
        // Grab the buffer into a local so it's not re-read every time, and loop through all the bytes.
        //

        auto buf = bufWorking;
        auto previousSample = buf[0];
        for (auto i = 0; i < bufWorkingBytes; i++) {
            auto sample = buf[i];

            //
            // Perform a sample rate conversion, because our ADC's 13.5 MHz clock won't precisely
            // match the video source, and as a result each line won't be an exact integral numbe
            // of samples. This current algorithm just skips or repeats a pixel every so often in
            // a form of nearest-neighbor interpolation.
            //
            // It's fast, but results in visible banding down the edges of the picture.
            //

            if (pxUntilSkip > 0) pxUntilSkip--;
            if (pxUntilSkip == 0) {
                pxUntilSkip = pxPerSkip;
                if (skipPixels) {
                    framex--;
                }
                else {
                    framex++;
                }
            }

            //
            // Measure the frequency of the incoming video source by keeping an average of the times between syncs.
            //

            // Ideally this sync detection should use a low-pass filtered version of the incoming signal so that
            // small bits of noise don't falsly trigger a sync.
            if ((previousSample >= 30) && (sample < 30)) {

                // A falling edge was observed. This could potentially be the start of a sync. To fliter out noise,
                // as well as avoid having to deal with the half-syncs that occur during the vertical blanking interval,
                // throw this sync out if it's too soon after the previous one.

                // During the vertical blanking interval, syncs occur at 2x speed. To avoid triggering on those, require
                // the previous sync to be 3/4s of an expected duration away or further.
                if ((pxcnt - previousSync) > IMG_WIDTH - IMG_WIDTH / 4) {
                    int syncDuration = pxcnt - previousSync;
                    auto idx = syncDurationsIdx++ % _countof(syncDurations);
                    syncDurations[idx] = syncDuration;
                    previousSync = pxcnt;
                }
            }


            if (framex >= IMG_WIDTH) {

                // We're expecting a sync to occur exactly every IMG_WIDTH samples.
                // The difference between what we measure and that value tells us how much we need to resample the signal.
                // Moving averages aren't that expensive, but still, we don't need to reevaluate this all that often as
                // the clock shouldn't drift very far.
                // So, just check every time we frame a new line.
                int acc = 0;
                for (int j = 0; j < _countof(syncDurations); j++) {
                    acc += syncDurations[j];
                }
                double avgSyncDuration = acc / 8.0;
                double percentError = avgSyncDuration / IMG_WIDTH;

                if (skipLine) {
                    if (lineDirection) {
                        framey++;
                    }
                    else {
                        framey--;
                    }
                    skipLine = false;
                }

                framey++;
                framex = 0;
                //if (sample > 30) {
                //    framex = IMG_WIDTH - 1;
                //}
                //else {
                //    framey++;
                //    framex = 0;
                //}
                //if (sample > 30) {
                //	int delta = syncX - (IMG_WIDTH / 2);
                //	pxPerSkip -= delta/300;
                //}
            }

            // If we have accumulated an entire TempFrame worth of data, copy it over to a DisplayFrame
            // for presentation.

            // This mess handles the fact that the incoming data is interlaced and draws every other line.
            // Therefore, once an entire set of every-other-lines is accumulated into TempBuffer
            // (called a "field" in TV terms), copy it into a DisplayFrame spaced out properly.
            // See this page https://www.radios-tv.co.uk/Pembers/World-TV-Standards/Line-Standards.html#LFSync
            // for information on why one field starts halfway through the middle of a line, but the tldr is
            // there are 525 lines and each field draws 262.5 lines.

            if ((fieldAlternator && (framey == (IMG_HEIGHT - 1)) && (framex == (IMG_WIDTH / 2))) ||
                (!fieldAlternator && (framey == IMG_HEIGHT))) {

                // For each line in TempFrame...
                for (int y = 0; y < IMG_HEIGHT; y++) {
                    // Copy it into the current DisplayFrame in the ring buffer...
                    memcpy(&DisplayFrames[FrameProduced % NUM_DISPLAY_FRAMES]

                        // spaced every other line, starting at either 0 or 1, alternating every other field.
                        [(y * 2 + fieldAlternator) * IMG_WIDTH],

                        &TempFrame[y * IMG_WIDTH],
                        IMG_WIDTH * sizeof(RGBQUAD));

                    // Do this for the next frame as well. That way, when we come around to the next frame,
                    // it will already have the previous field's data stored in the other lines.
                    memcpy(&DisplayFrames[(FrameProduced + 1) % NUM_DISPLAY_FRAMES]

                        [(y * 2 + fieldAlternator) * IMG_WIDTH],

                        &TempFrame[y * IMG_WIDTH],
                        IMG_WIDTH * sizeof(RGBQUAD));
                }

                int producedFrame = FrameProduced % NUM_DISPLAY_FRAMES;
                ULONG p = FrameProduced;
                TraceLoggingWrite(hTrace, "Produce", TraceLoggingValue(p, "Number"), TraceLoggingValue(producedFrame, "ModNumber"));

                // Release the frame to the display thread by incrementing the ring buffer's tail.
                FrameProduced++;
                framey = 0;
                // And alternate whether the lines start at 0 or 1 each frame.
                fieldAlternator = !fieldAlternator;
            }

            // Convert the incoming pixel samples into RGB values. For now, just make
            // them grayscale by copying their value into the R G and B values the same.
            auto frame_pixel = &TempFrame[framey * IMG_WIDTH + framex];
            frame_pixel->rgbRed = sample;
            frame_pixel->rgbGreen = sample;
            frame_pixel->rgbBlue = sample;

            framex++;
            pxcnt++;
            previousSample = sample;
        }
        TraceLoggingWrite(hTrace, "FrameEnd");
        bufDone = 1;
    }
}

volatile int proc_amp_offset = 40;
volatile int proc_amp_gain = 200;

double ConvertToFloat(UCHAR Sample) {
    double scaled = Sample;
    scaled = scaled - proc_amp_offset;
    if (scaled < 0) scaled = 0;
    scaled = scaled / proc_amp_gain;
    if (scaled > 1) scaled = 1;
    return scaled;
}

#define PI 3.14159265358979323846

#define SINCOS_TABLE_VALUES 128

double sin_table[SINCOS_TABLE_VALUES];
double cos_table[SINCOS_TABLE_VALUES];

void compute_sin_table() {
    double scale_factor = 2 * PI / SINCOS_TABLE_VALUES;
    for (int i = 0; i < SINCOS_TABLE_VALUES; i++) {
        sin_table[i] = sin(scale_factor * i);
    }
}

void compute_cos_table() {
    double scale_factor = 2 * PI / SINCOS_TABLE_VALUES;
    for (int i = 0; i < SINCOS_TABLE_VALUES; i++) {
        cos_table[i] = cos(scale_factor * i);
    }
}


double lookup_sin(double angle) {
    double scale_factor = SINCOS_TABLE_VALUES / (2 * PI);
    if (angle > 2 * PI) {
        int fold_times = int(angle / (2 * PI));
        angle = angle - (fold_times * 2 * PI);
    }
    int angle_index = (int)(angle * scale_factor) % SINCOS_TABLE_VALUES;
    return sin_table[angle_index];
}

double lookup_cos(double angle) {
    double scale_factor = SINCOS_TABLE_VALUES / (2 * PI);
    if (angle > 2 * PI) {
        int fold_times = int(angle / (2 * PI));
        angle = angle - (fold_times * 2 * PI);
    }
    int angle_index = (int)(angle * scale_factor) % SINCOS_TABLE_VALUES;
    return cos_table[angle_index];
}

#define PHASE_POSSIBLITIES 16

#ifndef SAMPLEFILTER_H_
#define SAMPLEFILTER_H_

/*

FIR filter designed with
 http://t-filter.appspot.com

sampling frequency: 13500000 Hz

* 0 Hz - 600000 Hz
  gain = 1
  desired ripple = 5 dB
  actual ripple = 3.3432506956360406 dB

* 1200000 Hz - 6750000 Hz
  gain = 0
  desired attenuation = -40 dB
  actual attenuation = -41.87297349898105 dB

*/

#define SAMPLEFILTER_TAP_NUM 31

typedef struct {
	double history[SAMPLEFILTER_TAP_NUM];
	unsigned int last_index;
} SampleFilter;

void SampleFilter_init(SampleFilter* f);
void SampleFilter_put(SampleFilter* f, double input);
double SampleFilter_get(SampleFilter* f);

#endif

static double filter_taps[SAMPLEFILTER_TAP_NUM] = {
  -0.006936820796447485,
  -0.0069779588191022065,
  -0.0089457883126518,
  -0.00973359215211557,
  -0.008498796723041713,
  -0.0044411858651625825,
  0.0030234913473887954,
  0.014134313735346522,
  0.028677656244440037,
  0.04593555922338768,
  0.06471960862660268,
  0.08349081873711689,
  0.10052957322283873,
  0.11414428920107425,
  0.12294390781238003,
  0.12598147692568096,
  0.12294390781238003,
  0.11414428920107425,
  0.10052957322283873,
  0.08349081873711689,
  0.06471960862660268,
  0.04593555922338768,
  0.028677656244440037,
  0.014134313735346522,
  0.0030234913473887954,
  -0.0044411858651625825,
  -0.008498796723041713,
  -0.00973359215211557,
  -0.0089457883126518,
  -0.0069779588191022065,
  -0.006936820796447485
};

void SampleFilter_init(SampleFilter* f) {
	int i;
	for (i = 0; i < SAMPLEFILTER_TAP_NUM; ++i)
		f->history[i] = 0;
	f->last_index = 0;
}

void SampleFilter_put(SampleFilter* f, double input) {
	f->history[f->last_index++] = input;
	if (f->last_index == SAMPLEFILTER_TAP_NUM)
		f->last_index = 0;
}

double SampleFilter_get(SampleFilter* f) {
	double acc = 0;
	int index = f->last_index, i;
	for (i = 0; i < SAMPLEFILTER_TAP_NUM; ++i) {
		index = index != 0 ? index - 1 : SAMPLEFILTER_TAP_NUM - 1;
		acc += f->history[index] * filter_taps[i];
	};
	return acc;
}

#define SAMPLEFILTER_TAP_NUM2 11

typedef struct {
	double history[SAMPLEFILTER_TAP_NUM2];
	unsigned int last_index;
} SampleFilter2;

void SampleFilter_init2(SampleFilter2* f);
void SampleFilter_put2(SampleFilter2* f, double input);
double SampleFilter_get2(SampleFilter2* f);

static double filter_taps2[SAMPLEFILTER_TAP_NUM2] = {
  -0.02562146715519146,
  -0.046196411100127226,
  -0.0027393208124534825,
  0.14105068599228346,
  0.31999670771558225,
  0.4025051434015828,
  0.31999670771558225,
  0.14105068599228346,
  -0.0027393208124534825,
  -0.046196411100127226,
  -0.02562146715519146
};

void SampleFilter_init2(SampleFilter2* f) {
	int i;
	for (i = 0; i < SAMPLEFILTER_TAP_NUM2; ++i)
		f->history[i] = 0;
	f->last_index = 0;
}

void SampleFilter_put2(SampleFilter2* f, double input) {
	f->history[f->last_index++] = input;
	if (f->last_index == SAMPLEFILTER_TAP_NUM2)
		f->last_index = 0;
}

double SampleFilter_get2(SampleFilter2* f) {
	double acc = 0;
	int index = f->last_index, i;
	for (i = 0; i < SAMPLEFILTER_TAP_NUM2; ++i) {
		index = index != 0 ? index - 1 : SAMPLEFILTER_TAP_NUM2 - 1;
		acc += f->history[index] * filter_taps2[i];
	};
	return acc;
}

//
// Right. So even leaving the filtering aside, it's not fast enough to do an char to double and demodulate and back
// on the main thread, which is kind of dissapointing. We know we have more CPU utilization avalible tho, because we're only
// using 30% of the total processing power. So, we can just spawn a few threads.
// What's the best way to do that?
// Well, I guess we could have a pipeline where after data comes in from the USB thread, we run it through the demodulator
// and dump it into two different temporary buffers.
// Then we run those through three different filtering threads to do the filtering, dumping those into three temporary buffers.
// Then we have a final thread which does the recombination back to chars, perhaps as part of the framing thread.
// Yeah that should work.
// Goodness this process is slow.
// It makes you appreciate how much faster a special purpose core, or even special purpose code, can be.
//

/*
DWORD WINAPI demodulate_thread(_In_ LPVOID lpParameter) {
	ULONG64 pxcnt = 0;

	while (true) {
		WaitForSingleObject(bufWorkingReady, INFINITE);
		TraceLoggingWrite(hTrace, "DemodBegin");
		PUCHAR buf = (PUCHAR)bufWorking;
		int num_samples_this_buffer = bufWorkingBytes;
		demod_len = num_samples_this_buffer;
		for (auto i = 0; i < num_samples_this_buffer; i++) {
			auto y = buf[i];

			double rate = (315.0 / 88.0) / 13.5;
			double deg33 = 25.0 * PI / 60.0;
			double phase_offset = 2 * PI * best_idx / PHASE_POSSIBLITIES;
			short cos_osc = (short)(lookup_cos(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024);
			short sin_osc = (short)(-1 * lookup_sin(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024);

			short u = cos_osc * y;
			short v = sin_osc * y;

			demod[i * 2] = u;
			demod[i * 2 + 1] = y;
		}

		TraceLoggingWrite(hTrace, "DemodEnd");
		SetEvent(demod_ready);
	}

	return 0;
}

typedef struct _filter_data {

} filter_data;

HANDLE filteru_ready;
HANDLE filterv_ready;
HANDLE filtery_ready;

DWORD WINAPI filters_thread(_In_ LPVOID lpParameter) {
	static unsigned long long pxcnt;
	while (true) {
		WaitForSingleObject(demod_ready, INFINITE);
		TraceLoggingWrite(hTrace, "Filters");
		PUCHAR buf = (PUCHAR)bufWorking;
		int num_samples_this_buffer = bufWorkingBytes;
		demod_len = num_samples_this_buffer;
		for (auto i = 0; i < num_samples_this_buffer; i++) {
			auto y = buf[i];

			double rate = (315.0 / 88.0) / 13.5;
			double deg33 = 25.0 * PI / 60.0;
			double phase_offset = 2 * PI * best_idx / PHASE_POSSIBLITIES;
			short cos_osc = (short)(lookup_cos(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024);
			short sin_osc = (short)(-1 * lookup_sin(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024);

			short u = cos_osc * y;
			short v = sin_osc * y;

			demod[i * 2] = u;
			demod[i * 2 + 1] = y;
		}

		TraceLoggingWrite(hTrace, "DemodEnd");
		SetEvent(demod_ready);
	}

	return 0;
}
*/

void decode_rgb(double y, double* r, double* g, double* b)
{
    static int pxUntilSkip = 0;
    static ULONG64 pxcnt;
    if (pxUntilSkip > 0) pxUntilSkip--;
    if (pxUntilSkip == 0) {
        pxUntilSkip = cpxPerSkip;
        if (cskipPixels) {
            pxcnt--;
        }
        else {
            pxcnt++;
        }
    }

	static SampleFilter f1,f2;
	static SampleFilter2 f3;
	static bool initialized;
	if (!initialized) {
		SampleFilter_init(&f1);
		SampleFilter_init(&f2);
		SampleFilter_init2(&f3);
	}
	initialized = true;

    double rate = (315.0 / 88.0) / 13.5;
    double deg33 = 25.0 * PI / 60.0;
    //double deg33 = 11.0 * PI / 60.0;
    double phase_offset = 2 * PI * best_idx / PHASE_POSSIBLITIES;
    double cos_osc = lookup_cos(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024;
    double sin_osc = -1 * lookup_sin(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024;

    double u = cos_osc * y;
    double v = sin_osc * y;
    //printf("y %f cos %f u %f y %f sin %f v %f\n", y, cos_osc, u, y, sin_osc, v);

    static Average_Ctx_Flt ctx_u, ctx_v;

	SampleFilter_put(&f1, u); double uf = SampleFilter_get(&f1) / 512;
	SampleFilter_put(&f2, v); double vf = SampleFilter_get(&f2) / 512;
	SampleFilter_put2(&f3, y); double yf = SampleFilter_get2(&f3);
	//y = yf;
	//uf /= 100;
	//vf /= 100;
	//double uf = u / 100;
	//double vf = v / 100;

	//double uf = MovingAvg8Flt(&ctx_u, u) / 2;
	//double vf = MovingAvg8Flt(&ctx_v, v) / 2;
	//double yf = y;

	//double yf = compute1(&buf3[pxcnt % 1024]);
    //printf("yf %f u %f v %f\n", yf, uf, vf);
    //ydelay[ydelay_idx] = y;
    //ydelay_idx = (ydelay_idx + 1) % 38;
    //y = ydelay[ydelay_idx];

    // # Given y and scaled u and v, turn back into RGB.
    *r = vf / 0.877 + y;
    *b = uf / 0.493 + y;
    *g = -0.509 * (*r - y) - 0.194 * (*b - y) + y;
    //*r = *b = *g = y;
    pxcnt++;
}


void decode_rgb2(double y, double* r, double* g, double* b)
{
    static int pxUntilSkip = 0;
    static ULONG64 pxcnt;
	static Average_Ctx_Flt ctx_u, ctx_v;
    if (pxUntilSkip > 0) pxUntilSkip--;
    if (pxUntilSkip == 0) {
        pxUntilSkip = cpxPerSkip;
        if (cskipPixels) {
            pxcnt--;
        }
        else {
            pxcnt++;
        }
    }


    double rate = (315.0 / 88.0) / 13.5;
    double deg33 = 25.0 * PI / 60.0;
    //double deg33 = 11.0 * PI / 60.0;
    double phase_offset = 2 * PI * best_idx / PHASE_POSSIBLITIES;
    double cos_osc = lookup_cos(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024;
    double sin_osc = -1 * lookup_sin(2 * PI * pxcnt * rate + phase_offset - deg33) * 1024;

    double u = cos_osc * y;
    double v = sin_osc * y;

	double uf = MovingAvg8Flt(&ctx_u, u);
	double vf = MovingAvg8Flt(&ctx_v, v);
	double yf = y;

    *r = vf / 0.877 + y;
    *b = uf / 0.493 + y;
    *g = -0.509 * (*r - y) - 0.194 * (*b - y) + y;
    pxcnt++;
}


void scale_rgb_to_char(double r, double g, double b, BYTE* rc, BYTE* gc, BYTE* bc) {

#define SCALE_FACTOR 256
    int aa = (int)(r * SCALE_FACTOR);
    int bb = (int)(g * SCALE_FACTOR);
    int cc = (int)(b * SCALE_FACTOR);

    if (aa > 255) aa = 255;
    if (aa < 0) aa = 0;
    if (bb > 255) bb = 255;
    if (bb < 0) bb = 0;
    if (cc > 255) cc = 255;
    if (cc < 0) cc = 0;

    *rc = aa;
    *gc = bb;
    *bc = cc;
}

RGBQUAD HandleSample(UCHAR Sample) {
    double s = ConvertToFloat(Sample);
    
    double r, g, b;
    decode_rgb(s, &r, &g, &b);
	//r = g = b = s;

    RGBQUAD q;
    scale_rgb_to_char(r, g, b, &q.rgbRed, &q.rgbGreen, &q.rgbBlue);

    return q;
}

DWORD WINAPI demodulate2_thread(_In_ LPVOID lpParameter) {
    while (true) {
        WaitForSingleObject(bufWorkingReady, INFINITE);
        TraceLoggingWrite(hTrace, "DemodBegin");
        auto bufIn = bufWorking;
        auto bufOut = bufDemodWorking;
        int num_samples_this_buffer = bufWorkingBytes;
        for (auto i = 0; i < num_samples_this_buffer; i++) {
            auto x = bufIn[i];
            auto y = HandleSample(x);
            bufOut[i] = y;
        }

        TraceLoggingWrite(hTrace, "DemodEnd");

        // Swap buffers and signal to other thread it's ready to go.
        auto temp = bufDemod;
        bufDemod = bufDemodWorking;
        bufDemodWorking = temp;
        bufDemodBytes = num_samples_this_buffer;

        SetEvent(bufDemodReady);
    }
    return 0;
}

DWORD WINAPI dwm_waiter_thread(_In_ LPVOID lpParameter) {

	UNREFERENCED_PARAMETER(lpParameter);

	while (true) {
		DwmFlush();
		TraceLoggingWrite(hTrace, "IssueRepaint");
		InvalidateRect(hMainWnd, NULL, FALSE);
	}

	return 0;
}

void ErrorExit(LPTSTR lpszFunction) {
	// Retrieve the system error message for the last-error code

	LPVOID lpMsgBuf;
	LPVOID lpDisplayBuf;
	DWORD dw = GetLastError();

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)& lpMsgBuf,
		0, NULL);

	// Display the error message and exit the process

	lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT,
		(lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) * sizeof(TCHAR));
	StringCchPrintf((LPTSTR)lpDisplayBuf,
		LocalSize(lpDisplayBuf) / sizeof(TCHAR),
		TEXT("%s failed with error %d: %s"),
		lpszFunction, dw, lpMsgBuf);
	MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);

	LocalFree(lpMsgBuf);
	LocalFree(lpDisplayBuf);
	ExitProcess(dw);
}