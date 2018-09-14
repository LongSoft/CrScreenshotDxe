/* CrScreenshotDxe.c

Copyright (c) 2016, Nikolaj Schlej, All rights reserved.

Redistribution and use in source and binary forms, 
with or without modification, are permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice, 
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice, 
  this list of conditions and the following disclaimer in the documentation 
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, 
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/SimpleFileSystem.h>

#include "lodepng.h" //PNG encoding library
#include "AppleEventMin.h" // Mac-specific keyboard input

STATIC
EFI_GUID
mAppleEventProtocolGuid = APPLE_EVENT_PROTOCOL_GUID;

EFI_STATUS
EFIAPI
FindWritableFs (
    OUT EFI_FILE_PROTOCOL **WritableFs
    )
{
    EFI_HANDLE *HandleBuffer = NULL;
    UINTN      HandleCount;
    UINTN      i;
    
    // Locate all the simple file system devices in the system
    EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &HandleBuffer);
    if (!EFI_ERROR (Status)) {
        EFI_FILE_PROTOCOL *Fs = NULL;
        // For each located volume
        for (i = 0; i < HandleCount; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs = NULL;
            EFI_FILE_PROTOCOL *File = NULL;
            
            // Get protocol pointer for current volume
            Status = gBS->HandleProtocol(HandleBuffer[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **) &SimpleFs);
            if (EFI_ERROR (Status)) {
                DEBUG((-1, "FindWritableFs: gBS->HandleProtocol[%d] returned %r\n", i, Status));
                continue;
            }
            
            // Open the volume
            Status = SimpleFs->OpenVolume(SimpleFs, &Fs);
            if (EFI_ERROR (Status)) {
                DEBUG((-1, "FindWritableFs: SimpleFs->OpenVolume[%d] returned %r\n", i, Status));
                continue;
            }
            
            // Try opening a file for writing
            Status = Fs->Open(Fs, &File, L"crsdtest.fil", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
            if (EFI_ERROR (Status)) {
                DEBUG((-1, "FindWritableFs: Fs->Open[%d] returned %r\n", i, Status));
                continue;
            }
            
            // Writable FS found
            Fs->Delete(File);
            *WritableFs = Fs;
            Status = EFI_SUCCESS;
            break;
        }
    }
    
    // Free memory
    if (HandleBuffer) {
        gBS->FreePool(HandleBuffer);
    }
    
    return Status;
}

EFI_STATUS
EFIAPI
ShowStatus (
    IN UINT8 Red, 
    IN UINT8 Green, 
    IN UINT8 Blue
    )
{
    // Determines the size of status square
    #define STATUS_SQUARE_SIDE 5

    UINTN        HandleCount;
    EFI_HANDLE   *HandleBuffer = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *GraphicsOutput = NULL;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Square[STATUS_SQUARE_SIDE * STATUS_SQUARE_SIDE];
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Backup[STATUS_SQUARE_SIDE * STATUS_SQUARE_SIDE];
    UINTN i;
    
    // Locate all instances of GOP
    EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR (Status)) {
        DEBUG((-1, "ShowStatus: Graphics output protocol not found\n"));
        return EFI_UNSUPPORTED;
    }
    
    // Set square color
    for (i = 0 ; i < STATUS_SQUARE_SIDE * STATUS_SQUARE_SIDE; i++) {
        Square[i].Blue = Blue;
        Square[i].Green = Green;
        Square[i].Red = Red;
        Square[i].Reserved = 0x00;
    }
    
    // For each GOP instance
    for (i = 0; i < HandleCount; i ++) {
        // Handle protocol
        Status = gBS->HandleProtocol(HandleBuffer[i], &gEfiGraphicsOutputProtocolGuid, (VOID **) &GraphicsOutput);
        if (EFI_ERROR (Status)) {
            DEBUG((-1, "ShowStatus: gBS->HandleProtocol[%d] returned %r\n", i, Status));
            continue;
        }
            
        // Backup current image
        GraphicsOutput->Blt(GraphicsOutput, Backup, EfiBltVideoToBltBuffer, 0, 0, 0, 0, STATUS_SQUARE_SIDE, STATUS_SQUARE_SIDE, 0);
        
        // Draw the status square
        GraphicsOutput->Blt(GraphicsOutput, Square, EfiBltBufferToVideo, 0, 0, 0, 0, STATUS_SQUARE_SIDE, STATUS_SQUARE_SIDE, 0);
        
        // Wait 500ms
        gBS->Stall(500*1000);
        
        // Restore the backup
        GraphicsOutput->Blt(GraphicsOutput, Backup, EfiBltBufferToVideo, 0, 0, 0, 0, STATUS_SQUARE_SIDE, STATUS_SQUARE_SIDE, 0);
    }
    
    return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
TakeScreenshot (
    IN EFI_KEY_DATA *KeyData
    )
{
    EFI_FILE_PROTOCOL *Fs = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *GraphicsOutput = NULL;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Image = NULL;
    UINTN      ImageSize;         // Size in pixels
    UINT8      *PngFile = NULL;
    UINTN      PngFileSize;       // Size in bytes
    EFI_STATUS Status;
    UINTN      HandleCount;
    EFI_HANDLE *HandleBuffer = NULL;
    UINT32     ScreenWidth;
    UINT32     ScreenHeight;
    CHAR16     FileName[8+1+3+1]; // 0-terminated 8.3 file name
    EFI_TIME   Time;
    UINTN      i, j;

    // Find writable FS
    Status = FindWritableFs(&Fs);
    if (EFI_ERROR (Status)) {
        DEBUG((-1, "TakeScreenshot: Can't find writable FS\n"));
        ShowStatus(0xFF, 0xFF, 0x00); //Yellow
        return EFI_SUCCESS;
    }
    
    // Locate all instances of GOP
    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR (Status)) {
        DEBUG((-1, "ShowStatus: Graphics output protocol not found\n"));
        return EFI_SUCCESS;
    }
    
    // For each GOP instance
    for (i = 0; i < HandleCount; i++) {
        do { // Break from do used instead of "goto error"
            // Handle protocol
            Status = gBS->HandleProtocol(HandleBuffer[i], &gEfiGraphicsOutputProtocolGuid, (VOID **) &GraphicsOutput);
            if (EFI_ERROR (Status)) {
                DEBUG((-1, "ShowStatus: gBS->HandleProtocol[%d] returned %r\n", i, Status));
                break;
            }
        
            // Set screen width, height and image size in pixels
            ScreenWidth  = GraphicsOutput->Mode->Info->HorizontalResolution;
            ScreenHeight = GraphicsOutput->Mode->Info->VerticalResolution;
            ImageSize = ScreenWidth * ScreenHeight;
            
            // Get current time
            Status = gRT->GetTime(&Time, NULL);
            if (!EFI_ERROR(Status)) {
                // Set file name to current day and time
                UnicodeSPrint(FileName, 26, L"%02d%02d%02d%02d.png", Time.Day, Time.Hour, Time.Minute, Time.Second);
            }
            else {
                // Set file name to scrnshot.png
                UnicodeSPrint(FileName, 26, L"scrnshot.png");
            }
            
            // Allocate memory for screenshot
            Status = gBS->AllocatePool(EfiBootServicesData, ImageSize * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL), (VOID **)&Image);
            if (EFI_ERROR(Status)) {
                DEBUG((-1, "TakeScreenshot: gBS->AllocatePool returned %r\n", Status));
                break;
            }
        
            // Take screenshot
            Status = GraphicsOutput->Blt(GraphicsOutput, Image, EfiBltVideoToBltBuffer, 0, 0, 0, 0, ScreenWidth, ScreenHeight, 0);
            if (EFI_ERROR(Status)) {
                DEBUG((-1, "TakeScreenshot: GraphicsOutput->Blt returned %r\n", Status));
                break;
            }
            
            // Check for pitch black image (it means we are using a wrong GOP)
            for (j = 0; j < ImageSize; j++) {
                if (Image[j].Red != 0x00 || Image[j].Green != 0x00 || Image[j].Blue != 0x00)
                    break;
            }
            if (j == ImageSize) {
                DEBUG((-1, "TakeScreenshot: GraphicsOutput->Blt returned pitch black image, skipped\n"));
                ShowStatus(0x00, 0x00, 0xFF); //Blue
                break;
            }
            
            // Open or create output file
            Status = Fs->Open(Fs, &File, FileName, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
            if (EFI_ERROR (Status)) {
                DEBUG((-1, "TakeScreenshot: Fs->Open of %s returned %r\n", FileName, Status));
                break;
            }
            
            // Convert BGR to RGBA with Alpha set to 0xFF
            for (j = 0; j < ImageSize; j++) {
                UINT8 Temp = Image[j].Blue;
                Image[j].Blue = Image[j].Red;
                Image[j].Red = Temp;
                Image[j].Reserved = 0xFF;
            }
        
            // Encode raw RGB image to PNG format
            j = lodepng_encode32(&PngFile, &PngFileSize, (CONST UINT8*)Image, ScreenWidth, ScreenHeight);
            if (j) {
                DEBUG((-1, "TakeScreenshot: lodepng_encode32 returned %d\n", j));
                break;
            }
                
            // Write PNG image into the file and close it
            Status = File->Write(File, &PngFileSize, PngFile);
            File->Close(File);
            if (EFI_ERROR(Status)) {
                DEBUG((-1, "TakeScreenshot: File->Write returned %r\n", Status));
                break;
            }
            
            // Show success
            ShowStatus(0x00, 0xFF, 0x00); //Green
        } while(0);
        
        // Free memory
        if (Image)
            gBS->FreePool(Image);
        if (PngFile)
            lodepng_free(PngFile);
        Image = NULL;
        PngFile = NULL;
    }
    
    // Show error
    if (EFI_ERROR(Status))
        ShowStatus(0xFF, 0x00, 0x00); //Red
    
    return EFI_SUCCESS;
}

VOID
EFIAPI
AppleEventKeyHandler (
  IN APPLE_EVENT_INFORMATION  *Information,
  IN VOID                     *NotifyContext
  )
{
    // Mark the context argument as used
    (VOID) NotifyContext;

    // Ignore invalid information if it happened to arrive
    if (Information == NULL || (Information->EventType & APPLE_EVENT_TYPE_KEY_UP) == 0) {
        return;
    }

    // Apple calls ALT key by the name of OPTION key
    if (Information->KeyData->InputKey.ScanCode == SCAN_F12
        && Information->Modifiers == (APPLE_MODIFIER_LEFT_CONTROL|APPLE_MODIFIER_LEFT_OPTION)) {
        // Take a screenshot
        TakeScreenshot (NULL);
    }
}

EFI_STATUS
EFIAPI
CrScreenshotDxeEntry (
    IN EFI_HANDLE         ImageHandle,
    IN EFI_SYSTEM_TABLE   *SystemTable
    )
{
    EFI_STATUS                        Status;
    UINTN                             HandleCount = 0;
    EFI_HANDLE                        *HandleBuffer = NULL;
    UINTN                             Index;
    EFI_KEY_DATA                      SimpleTextInExKeyStroke;
    EFI_HANDLE                        SimpleTextInExHandle;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *SimpleTextInEx;
    APPLE_EVENT_HANDLE                AppleEventHandle;
    APPLE_EVENT_PROTOCOL              *AppleEvent;
    BOOLEAN                           Installed = FALSE;

    // Set keystroke to be LCtrl+LAlt+F12
    SimpleTextInExKeyStroke.Key.ScanCode = SCAN_F12;
    SimpleTextInExKeyStroke.Key.UnicodeChar = 0;
    SimpleTextInExKeyStroke.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED | EFI_LEFT_ALT_PRESSED;
    SimpleTextInExKeyStroke.KeyState.KeyToggleState = 0;

    // Locate compatible protocols, firstly try SimpleTextInEx, otherwise use AppleEvent
    Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL, &HandleCount, &HandleBuffer);
    if (!EFI_ERROR (Status)) {
        // For each instance
        for (Index = 0; Index < HandleCount; Index++) {
            Status = gBS->HandleProtocol (HandleBuffer[Index], &gEfiSimpleTextInputExProtocolGuid, (VOID **) &SimpleTextInEx);

            // Get protocol handle
            if (EFI_ERROR (Status)) {
               DEBUG ((-1, "CrScreenshotDxeEntry: gBS->HandleProtocol[%d] SimpleTextInputEx returned %r\n", Index, Status));
               continue;
            }

            // Register key notification function
            Status = SimpleTextInEx->RegisterKeyNotify (
                    SimpleTextInEx,
                    &SimpleTextInExKeyStroke,
                    TakeScreenshot,
                    &SimpleTextInExHandle
                    );
            if (!EFI_ERROR (Status)) {
                Installed = TRUE;
            } else {
                DEBUG ((-1, "CrScreenshotDxeEntry: SimpleTextInEx->RegisterKeyNotify[%d] returned %r\n", Index, Status));
            }
        }
    } else {
        DEBUG((-1, "CrScreenshotDxeEntry: gBS->LocateHandleBuffer SimpleTextInputEx returned %r\n", Status));
        HandleBuffer = NULL;
        Status = gBS->LocateHandleBuffer (ByProtocol, &mAppleEventProtocolGuid, NULL, &HandleCount, &HandleBuffer);
        if (EFI_ERROR (Status)) {
            DEBUG ((-1, "CrScreenshotDxeEntry: gBS->LocateHandleBuffer AppleEvent returned %r\n", Status));
            return EFI_UNSUPPORTED;
        }

        // Traverse AppleEvent handles similarly to SimpleTextInputEx
        for (Index = 0; Index < HandleCount; Index++) {
            Status = gBS->HandleProtocol (HandleBuffer[Index], &mAppleEventProtocolGuid, (VOID **) &AppleEvent);

            // Get protocol handle
            if (EFI_ERROR (Status)) {
               DEBUG ((-1, "CrScreenshotDxeEntry: gBS->HandleProtocol[%d] AppleEvent returned %r\n", Index, Status));
               continue;
            }

            // Check protocol interface compatibility
            if (AppleEvent->Revision < APPLE_EVENT_PROTOCOL_REVISION) {
                DEBUG ((-1, "CrScreenshotDxeEntry: AppleEvent[%d] has outdated revision %d, expected %d\n",
                    Index, AppleEvent->Revision, APPLE_EVENT_PROTOCOL_REVISION));
                continue;
            }

            // Register key handler, which will later determine LCtrl+LAlt+F12 combination
            Status = AppleEvent->RegisterHandler (
                    APPLE_EVENT_TYPE_KEY_UP,
                    AppleEventKeyHandler,
                    &AppleEventHandle,
                    NULL
                    );
            if (!EFI_ERROR (Status)) {
                Installed = TRUE;
            } else {
                DEBUG((-1, "CrScreenshotDxeEntry: AppleEvent->RegisterHandler[%d] returned %r\n", Index, Status));
            }
        }
    }

    // Free memory used for handle buffer
    if (HandleBuffer) {
        gBS->FreePool(HandleBuffer);
    }

    // Show success only when we found at least one working implementation
    if (Installed) {
        ShowStatus(0xFF, 0xFF, 0xFF); //White
    }

    return EFI_SUCCESS;
}
