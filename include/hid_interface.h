/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

/*! \file hid_interface.h
    \brief XENHID HID Interface

    This interface provides access to the PV network frontend
*/

#ifndef _XENHID_HID_INTERFACE_H
#define _XENHID_HID_INTERFACE_H

#ifndef _WINDLL

/*! \typedef XENHID_HID_ACQUIRE
    \brief Acquire a reference to the HID interface

    \param Interface The interface header
*/  
typedef NTSTATUS
(*XENHID_HID_ACQUIRE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENHID_HID_RELEASE
    \brief Release a reference to the HID interface

    \param Interface The interface header
*/  
typedef VOID
(*XENHID_HID_RELEASE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENHID_HID_CALLBACK
    \brief Provider to subscriber callback function

    \param Argument An optional context argument passed to the callback
    \param Buffer A HID report buffer to complete
    \param Length The length of the \a Buffer
*/
typedef BOOLEAN
(*XENHID_HID_CALLBACK)(
    IN  PVOID       Argument OPTIONAL,
    IN  PVOID       Buffer,
    IN  ULONG       Length
    );

/*! \typedef XENHID_HID_ENABLE
    \brief Enable the HID interface

    All packets queued for transmit will be rejected and no packets will
    be queued for receive until this method completes. 

    \param Interface The interface header
    \param Callback The subscriber's callback function
    \param Argument An optional context argument passed to the callback
*/
typedef NTSTATUS
(*XENHID_HID_ENABLE)(
    IN  PINTERFACE          Interface,
    IN  XENHID_HID_CALLBACK Callback,
    IN  PVOID               Argument OPTIONAL
    );

/*! \typedef XENHID_HID_DISABLE
    \brief Disable the HID interface

    This method will not complete until any packets queued for receive
    have been returned. Any packets queued for transmit may be aborted.

    \param Interface The interface header
*/
typedef VOID
(*XENHID_HID_DISABLE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENHID_HID_GET_DEVICE_ATTRIBUTES
    \brief Get the HID Device Attributes structure

*/
typedef NTSTATUS
(*XENHID_HID_GET_DEVICE_ATTRIBUTES)(
    IN  PINTERFACE      Interface,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_GET_DEVICE_DESCRIPTOR
    \brief Get the HID Device Descriptor structure

    \param Interface The interface header
    \param Buffer The buffer to fill
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_DEVICE_DESCRIPTOR)(
    IN  PINTERFACE      Interface,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_GET_REPORT_DESCRIPTOR
    \brief Get the HID Report Descriptor structure

    \param Interface The interface header
    \param Buffer The buffer to fill
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_REPORT_DESCRIPTOR)(
    IN  PINTERFACE      Interface,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_GET_STRING
    \brief Get the HID Device Descriptor structure

    \param Interface The interface header
    \param Identifier The string identifier
    \param Buffer The buffer to fill
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_STRING)(
    IN  PINTERFACE      Interface,
    IN  ULONG           Identifier,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_GET_INDEXED_STRING
    \brief Set the HID Device Descriptor structure

    \param Interface The interface header
    \param Index The index of the string
    \param Buffer The buffer to fill
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_INDEXED_STRING)(
    IN  PINTERFACE      Interface,
    IN  ULONG           Index,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_GET_FEATURE
    \brief Get the feature

    \param Interface The interface header
    \param ReportId The report id to set
    \param Buffer The report buffer
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_FEATURE)(
    IN  PINTERFACE      Interface,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_SET_FEATURE
    \brief Set the feature report

    \param Interface The interface header
    \param ReportId The report id to set
    \param Buffer The report buffer
    \param Length The length of the buffer
*/
typedef NTSTATUS
(*XENHID_HID_SET_FEATURE)(
    IN  PINTERFACE      Interface,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length
    );

/*! \typedef XENHID_HID_GET_INPUT_REPORT
    \brief Get the input report

    \param Interface The interface header
    \param ReportId The report id to set
    \param Buffer The report buffer
    \param Length The length of the buffer
    \param Returned The number of bytes returned
*/
typedef NTSTATUS
(*XENHID_HID_GET_INPUT_REPORT)(
    IN  PINTERFACE      Interface,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    );

/*! \typedef XENHID_HID_SET_OUTPUT_REPORT
    \brief Set the output report

    \param Interface The interface header
    \param ReportId The report id to set
    \param Buffer The write report buffer
    \param Length The length of the buffer
*/
typedef NTSTATUS
(*XENHID_HID_SET_OUTPUT_REPORT)(
    IN  PINTERFACE      Interface,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length
    );

/*! \typedef XENHID_HID_READ_REPORT
    \brief Checks to see if any pending read reports
           need completing. A single read report will be 
           completed by calling the callback

    \param Interface The interface header
*/
typedef VOID
(*XENHID_HID_READ_REPORT)(
    IN  PINTERFACE      Interface
    );

/*! \typedef XENHID_HID_WRITE_REPORT
    \brief Set the output report

    \param Interface The interface header
    \param ReportId The report id to set
    \param Buffer The write report buffer
    \param Length The length of the buffer
*/
typedef NTSTATUS
(*XENHID_HID_WRITE_REPORT)(
    IN  PINTERFACE      Interface,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length
    );

// {D215E1B5-8C38-420A-AEA6-02520DF3A621}
DEFINE_GUID(GUID_XENHID_HID_INTERFACE,
0xd215e1b5, 0x8c38, 0x420a, 0xae, 0xa6, 0x2, 0x52, 0xd, 0xf3, 0xa6, 0x21);

/*! \struct _XENHID_HID_INTERFACE_V1
    \brief HID interface version 1
    \ingroup interfaces
*/
struct _XENHID_HID_INTERFACE_V1 {
    INTERFACE                                       Interface;
    XENHID_HID_ACQUIRE                              Acquire;
    XENHID_HID_RELEASE                              Release;
    XENHID_HID_ENABLE                               Enable;
    XENHID_HID_DISABLE                              Disable;
    XENHID_HID_GET_DEVICE_ATTRIBUTES                GetDeviceAttributes;
    XENHID_HID_GET_DEVICE_DESCRIPTOR                GetDeviceDescriptor;
    XENHID_HID_GET_REPORT_DESCRIPTOR                GetReportDescriptor;
    XENHID_HID_GET_STRING                           GetString;
    XENHID_HID_GET_INDEXED_STRING                   GetIndexedString;
    XENHID_HID_GET_FEATURE                          GetFeature;
    XENHID_HID_SET_FEATURE                          SetFeature;
    XENHID_HID_GET_INPUT_REPORT                     GetInputReport;
    XENHID_HID_SET_OUTPUT_REPORT                    SetOutputReport;
    XENHID_HID_READ_REPORT                          ReadReport;
    XENHID_HID_WRITE_REPORT                         WriteReport;
};

typedef struct _XENHID_HID_INTERFACE_V1 XENHID_HID_INTERFACE, *PXENHID_HID_INTERFACE;

/*! \def XENHID_HID
    \brief Macro at assist in method invocation
*/
#define XENHID_HID(_Method, _Interface, ...)    \
    (_Interface)-> ## _Method((PINTERFACE)(_Interface), __VA_ARGS__)

#endif  // _WINDLL

#define XENHID_HID_INTERFACE_VERSION_MIN    1
#define XENHID_HID_INTERFACE_VERSION_MAX    1

#endif  // _XENHID_INTERFACE_H
