// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *    .,-:::::    .,::      .::::::::.    .,::      .:
// *  ,;;;'````'    `;;;,  .,;;  ;;;'';;'   `;;;,  .,;;
// *  [[[             '[[,,[['   [[[__[[\.    '[[,,[['
// *  $$$              Y$$$P     $$""""Y$$     Y$$$P
// *  `88bo,__,o,    oP"``"Yo,  _88o,,od8P   oP"``"Yo,
// *    "YUMMMMMP",m"       "Mm,""YUMMMP" ,m"       "Mm,
// *
// *   Cxbx->Win32->CxbxKrnl->EmuKrnlAv.cpp
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *  (c) 2016 Patrick van Logchem <pvanlogchem@gmail.com>
// *
// *  All rights reserved
// *
// ******************************************************************
#define _XBOXKRNL_DEFEXTRN_

#define LOG_PREFIX "KRNL"

// prevent name collisions
namespace xboxkrnl
{
	#include <xboxkrnl/xboxkrnl.h> // For AvGetSavedDataAddress, etc.
};

#include "Logging.h" // For LOG_FUNC()
#include "EmuKrnlLogging.h"

// prevent name collisions
namespace NtDll
{
#include "EmuNtDll.h"
};

#include "Emu.h" // For EmuWarning()
#include "EmuXTL.h"
#include "EmuX86.h"

#include "EmuKrnlAvModes.h"
#include "devices\video\nv2a_int.h"

#ifndef VOID
#define VOID void
#endif

// HW Register helper functions
xboxkrnl::UCHAR REG_RD08(VOID* Ptr, xboxkrnl::ULONG Addr)
{
	return EmuX86_Read((xbaddr)Ptr + Addr, sizeof(uint8_t));
}

VOID REG_WR08(VOID* Ptr, xboxkrnl::ULONG Addr, xboxkrnl::UCHAR Val)
{
	EmuX86_Write((xbaddr)Ptr + Addr, Val, sizeof(uint8_t));
}

xboxkrnl::ULONG REG_RD32(VOID* Ptr, xboxkrnl::ULONG Addr)
{
	return EmuX86_Read((xbaddr)Ptr + Addr, sizeof(uint32_t));
}

VOID REG_WR32(VOID* Ptr, xboxkrnl::ULONG Addr, xboxkrnl::ULONG Val)
{
	EmuX86_Write((xbaddr)Ptr + Addr, Val, sizeof(uint32_t));
}

VOID CRTC_WR(VOID* Ptr, xboxkrnl::UCHAR i, xboxkrnl::UCHAR d)
{
	REG_WR08(Ptr, NV_PRMCIO_CRX__COLOR, i);
	REG_WR08(Ptr, NV_PRMCIO_CR__COLOR, d);
}

VOID SRX_WR(VOID *Ptr, xboxkrnl::UCHAR i, xboxkrnl::UCHAR d)

{
	REG_WR08(Ptr, NV_PRMVIO_SRX, i);
	REG_WR08(Ptr, NV_PRMVIO_SR, (d));
}

VOID GRX_WR(VOID *Ptr, xboxkrnl::UCHAR i, xboxkrnl::UCHAR d)
{
	REG_WR08(Ptr, NV_PRMVIO_GRX, i);
	REG_WR08(Ptr, NV_PRMVIO_GX, (d));
}

VOID ARX_WR(VOID *Ptr, xboxkrnl::UCHAR i, xboxkrnl::UCHAR d)
{
	REG_WR08(Ptr, NV_PRMCIO_ARX, i);
	REG_WR08(Ptr, NV_PRMCIO_ARX, (d));
}




// Global Variable(s)
PVOID g_pPersistedData = NULL;
ULONG AvpCurrentMode = 0;

ULONG AvQueryAvCapabilities()
{
	// This is the only AV mode we currently emulate, so we can hardcode the return value
	// TODO: Once we add the ability to change av pack, read HalSmcVideoMode) and convert it to a AV_PACK_*
	ULONG avpack = AV_PACK_HDTV;
	ULONG type;
	ULONG resultSize;

	// First, read the factory AV settings
	ULONG avRegion;
	NTSTATUS result = xboxkrnl::ExQueryNonVolatileSetting(
		xboxkrnl::XC_FACTORY_AV_REGION,
		&type,
		&avRegion,
		sizeof(ULONG),
		&resultSize);

	// If this failed, default to AV_STANDARD_NTSC_M | AV_FLAGS_60Hz
	if (result != STATUS_SUCCESS || resultSize != sizeof(ULONG)) {
		avRegion = AV_STANDARD_NTSC_M | AV_FLAGS_60Hz;
	}

	// Read the user-configurable (via the dashboard) settings
	ULONG userSettings;
	result = xboxkrnl::ExQueryNonVolatileSetting(
		xboxkrnl::XC_VIDEO,
		&type,
		&userSettings,
		sizeof(ULONG),
		&resultSize);

	// If this failed, default to no user-options set
	if (result != STATUS_SUCCESS || resultSize != sizeof(ULONG)) {
		userSettings = 0;
	}

	return avpack | (avRegion & (AV_STANDARD_MASK | AV_REFRESH_MASK)) | (userSettings & ~(AV_STANDARD_MASK | AV_PACK_MASK));
}

xboxkrnl::PVOID xboxkrnl::AvSavedDataAddress = xbnullptr;

// ******************************************************************
// * 0x0001 - AvGetSavedDataAddress()
// ******************************************************************
XBSYSAPI EXPORTNUM(1) xboxkrnl::PVOID NTAPI xboxkrnl::AvGetSavedDataAddress(void)
{
	LOG_FUNC();

	RETURN(AvSavedDataAddress);
}

// ******************************************************************
// * 0x0002 - AvSendTVEncoderOption()
// ******************************************************************
XBSYSAPI EXPORTNUM(2) VOID NTAPI xboxkrnl::AvSendTVEncoderOption
(
	IN  PVOID   RegisterBase,
	IN  ULONG   Option,
	IN  ULONG   Param,
	OUT ULONG   *Result
)
{
	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(RegisterBase)
		LOG_FUNC_ARG(Option)
		LOG_FUNC_ARG(Param)
		LOG_FUNC_ARG_OUT(Result)
		LOG_FUNC_END;

	//if (RegisterBase == NULL)
	//	RegisterBase = (void *)NV20_REG_BASE_KERNEL;

	switch (Option) {
	case AV_OPTION_MACROVISION_MODE:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_ENABLE_CC:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_DISABLE_CC:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_SEND_CC_DATA:
		LOG_UNIMPLEMENTED();
		break;
	case AV_QUERY_CC_STATUS:
		LOG_UNIMPLEMENTED();
		break;
	case AV_QUERY_AV_CAPABILITIES:
		*Result = AvQueryAvCapabilities();
		break;
	case AV_OPTION_BLANK_SCREEN:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_MACROVISION_COMMIT:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_FLICKER_FILTER:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_ZERO_MODE:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_QUERY_MODE:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_ENABLE_LUMA_FILTER:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_GUESS_FIELD:
		LOG_UNIMPLEMENTED();
		break;
	case AV_QUERY_ENCODER_TYPE:
		LOG_UNIMPLEMENTED();
		break;
	case AV_QUERY_MODE_TABLE_VERSION:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_CGMS:
		LOG_UNIMPLEMENTED();
		break;
	case AV_OPTION_WIDESCREEN:
		LOG_UNIMPLEMENTED();
		break;
	default:
		// do nothing
		break;
	}
}

// Cached Display Mode format, used by NV2A to deermine framebuffer format
ULONG g_AvDisplayModeFormat = 0;

// ******************************************************************
// * 0x0003 - AvSetDisplayMode()
// ******************************************************************
XBSYSAPI EXPORTNUM(3) xboxkrnl::ULONG NTAPI xboxkrnl::AvSetDisplayMode
(
	IN  PVOID   RegisterBase,
	IN  ULONG   Step,
	IN  ULONG   Mode,
	IN  ULONG   Format,
	IN  ULONG   Pitch,
	IN  ULONG   FrameBuffer
)
{
	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(RegisterBase)
		LOG_FUNC_ARG(Step)
		LOG_FUNC_ARG(Mode)
		LOG_FUNC_ARG(Format)
		LOG_FUNC_ARG(Pitch)
		LOG_FUNC_ARG(FrameBuffer)
		LOG_FUNC_END;

	if (Mode == AV_MODE_OFF) {
		Mode = AV_MODE_640x480_TO_NTSC_M_YC	| AV_MODE_FLAGS_DACA_DISABLE | AV_MODE_FLAGS_DACB_DISABLE
			| AV_MODE_FLAGS_DACC_DISABLE | AV_MODE_FLAGS_DACD_DISABLE;
	}

	ULONG OutputMode = Mode & AV_MODE_OUT_MASK;
	ULONG iRegister = (Mode & 0x00FF0000) >> 16;
	ULONG iCRTC = (Mode & 0x0000FF00) >> 8;
	ULONG iTV = (Mode & 0x0000007F);
	UCHAR DACs = (UCHAR)((Mode & 0x0F000000) >> 24);

	ULONG GeneralControl = 0;
	UCHAR CR28Depth = 0;

	switch (Format)
	{
	case XTL::X_D3DFMT_LIN_A1R5G5B5:
	case XTL::X_D3DFMT_LIN_X1R5G5B5:
		GeneralControl = 0x00100030;
		CR28Depth = 2;
		break;
	case XTL::X_D3DFMT_LIN_R5G6B5:
		GeneralControl = 0x00101030;
		CR28Depth = 2;
		break;
	case XTL::X_D3DFMT_LIN_A8R8G8B8:
	case XTL::X_D3DFMT_LIN_X8R8G8B8:
		GeneralControl = 0x00100030;
		CR28Depth = 3;
		break;
	}

	// HACK: Store D3D format that was set, so we can decode it in nv2a swap
	// TODO: Fix this so nv2a state is used to get these values...
	g_AvDisplayModeFormat = Format;

	Pitch /= 8;

	if (AvpCurrentMode == Mode)
	{
		REG_WR32(RegisterBase, NV_PRAMDAC_GENERAL_CONTROL, GeneralControl);
		CRTC_WR(RegisterBase, NV_CIO_SR_LOCK_INDEX, NV_CIO_SR_UNLOCK_RW_VALUE);
		CRTC_WR(RegisterBase, 0x13, (UCHAR)(Pitch & 0xFF));
		CRTC_WR(RegisterBase, 0x19, (UCHAR)((Pitch & 0x700) >> 3));
		CRTC_WR(RegisterBase, 0x28, 0x80 | CR28Depth);
		REG_WR32(RegisterBase, NV_PCRTC_START, FrameBuffer);

		AvSendTVEncoderOption(RegisterBase, AV_OPTION_FLICKER_FILTER, 5, NULL);
		AvSendTVEncoderOption(RegisterBase, AV_OPTION_ENABLE_LUMA_FILTER, FALSE, NULL);
		AvpCurrentMode = Mode;

		RETURN(STATUS_SUCCESS);
	}

	// TODO: Lots of setup/TV encoder configuration
	// Ignored for now since we don't emulate that stuff yet...

	LOG_INCOMPLETE();

	REG_WR32(RegisterBase, NV_PRAMDAC_GENERAL_CONTROL, GeneralControl);

	const ULONG* pLong = AvpRegisters[iRegister];
	const ULONG* pLongMax = pLong + sizeof(AvpRegisters[0]) / sizeof(ULONG);

	for (long i = 0; pLong < pLongMax; pLong++, i++)	{
		REG_WR32(RegisterBase, AvpRegisters[0][i], *pLong);
	}

	if (Mode & AV_MODE_FLAGS_SCART)	{
		REG_WR32(RegisterBase, 0x680630, 0);
		REG_WR32(RegisterBase, 0x6808C4, 0);
		REG_WR32(RegisterBase, 0x68084C, 0);
	}

	const UCHAR* pByte = AvpSRXRegisters;
	const UCHAR* pByteMax = pByte + sizeof(AvpSRXRegisters);

	for (long i = 0; pByte < pByteMax; pByte++, i++) {
		SRX_WR(RegisterBase, (UCHAR)i, *pByte);
	}

	pByte = AvpGRXRegisters;
	pByteMax = pByte + sizeof(AvpGRXRegisters);

	for (long i = 0; pByte < pByteMax; pByte++, i++)	{
		GRX_WR(RegisterBase, (UCHAR)i, *pByte);
	}

	REG_RD08(RegisterBase, NV_PRMCIO_INP0__COLOR);

	pByte = AvpARXRegisters;
	pByteMax = pByte + sizeof(AvpARXRegisters);

	for (long i = 0; pByte < pByteMax; pByte++, i++)	{
		ARX_WR(RegisterBase, (UCHAR)i, *pByte);
	}

	REG_WR08(RegisterBase, NV_PRMCIO_ARX, 0x20);

	CRTC_WR(RegisterBase, 0x11, 0x00);
	pByte = AvpCRTCRegisters[iCRTC];
	pByteMax = pByte + sizeof(AvpCRTCRegisters[0]);

	for (long i = 0; pByte < pByteMax; pByte++, i++) {
		UCHAR Register = AvpCRTCRegisters[0][i];
		UCHAR Data = *pByte;

		if (Register == 0x13) {
			Data = (UCHAR)(Pitch & 0xFF);
		} else if (Register == 0x19) {
			Data |= (UCHAR)((Pitch & 0x700) >> 3);
		} else if (Register == 0x25) {
			Data |= (UCHAR)((Pitch & 0x800) >> 6);
		}

		CRTC_WR(RegisterBase, AvpCRTCRegisters[0][i], Data);
	}

	// TODO: More TV Encoder stuff...

	REG_WR32(RegisterBase, NV_PCRTC_START, FrameBuffer);
	AvpCurrentMode = Mode;

	RETURN(STATUS_SUCCESS);
}

// ******************************************************************
// * 0x0004 - AvSetSavedDataAddress()
// ******************************************************************
XBSYSAPI EXPORTNUM(4) VOID NTAPI xboxkrnl::AvSetSavedDataAddress
(
	IN  PVOID   Address
)
{
	LOG_FUNC_ONE_ARG(Address);

	AvSavedDataAddress = Address;
}