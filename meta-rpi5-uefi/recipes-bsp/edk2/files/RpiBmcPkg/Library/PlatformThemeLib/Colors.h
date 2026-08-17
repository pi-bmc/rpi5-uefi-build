/** @file
  Dark-theme color palette for the Setup browser.

  PlatformThemeLib's only intended divergence from the stock
  MdeModulePkg CustomizedDisplayLib it is forked from: every LIGHTGRAY
  surface goes black, with foregrounds re-picked for contrast on the
  dark ground. The four field colors the display engine reads through
  the PcdBrowser* PCDs are set to match in RpiBmc.dsc.inc (lightgray
  field text, white subtitles, black-on-cyan highlight) - the theme is
  split between this header and those PCDs by upstream design.

  Copyright (c) 2004 - 2011, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef _COLORS_H_
#define _COLORS_H_

//
// Screen Color Settings
//
#define PICKLIST_HIGHLIGHT_TEXT        EFI_WHITE
#define PICKLIST_HIGHLIGHT_BACKGROUND  EFI_BACKGROUND_BLACK
#define TITLE_TEXT                     EFI_WHITE
#define TITLE_BACKGROUND               EFI_BACKGROUND_BLACK
#define KEYHELP_TEXT                   EFI_LIGHTGRAY
#define KEYHELP_BACKGROUND             EFI_BACKGROUND_BLACK
#define SUBTITLE_BACKGROUND            EFI_BACKGROUND_BLACK
#define BANNER_TEXT                    EFI_LIGHTGRAY
#define BANNER_BACKGROUND              EFI_BACKGROUND_BLACK
#define FIELD_TEXT_GRAYED              EFI_DARKGRAY
#define FIELD_BACKGROUND               EFI_BACKGROUND_BLACK
#define POPUP_TEXT                     EFI_LIGHTGRAY
#define POPUP_BACKGROUND               EFI_BACKGROUND_BLACK
#define POPUP_INVERSE_TEXT             EFI_BLACK
#define POPUP_INVERSE_BACKGROUND       EFI_BACKGROUND_LIGHTGRAY
#define HELP_TEXT                      EFI_DARKGRAY
#define ERROR_TEXT                     EFI_RED | EFI_BRIGHT
#define INFO_TEXT                      EFI_LIGHTGRAY
#define ARROW_TEXT                     EFI_DARKGRAY
#define ARROW_BACKGROUND               EFI_BACKGROUND_BLACK

#endif
