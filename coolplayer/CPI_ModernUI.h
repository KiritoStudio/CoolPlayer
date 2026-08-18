/*
 * CoolPlayer - foobar2000-style native UI for the main window.
 *
 * This module owns the main window's client area. It hosts a panel of plain
 * Win32 common controls (transport buttons, seek and volume trackbars, a
 * report-mode ListView playlist, and a status bar) so the OS does all text
 * rendering, theming and hit-testing. Nothing is custom-painted.
 *
 * It reads state directly from globals/options/CPL_.../CPLI_... so there is
 * no separate UI state to keep in sync with the player/playlist engine.
 */
////////////////////////////////////////////////////////////////////////////////

#ifndef CPI_MODERNUI_H
#define CPI_MODERNUI_H

// Initial main window size (normal overlapped desktop window, resizable)
#define MODERNUI_WINDOW_WIDTH   780
#define MODERNUI_WINDOW_HEIGHT  520

// Seek range passed to CPI_Player__SetPositionRange - independent of any
// skin pixel geometry so seeking precision no longer depends on the skin.
#define MODERNUI_SEEK_RANGE     10000

void ModernUI_Init(HWND hWnd);
void ModernUI_Destroy(void);

// Main-window message helpers. Paint just fills the sliver of client area
// not covered by the control panel; OnSize keeps the panel sized to the
// client rectangle.
void ModernUI_Paint(HWND hWnd);
void ModernUI_OnSize(HWND hWnd);

// Legacy compatibility stubs - the native controls own all mouse
// interaction now, so these do nothing (kept so main.c needs no re-plumbing).
void ModernUI_OnLButtonDown(HWND hWnd, POINTS pt);
void ModernUI_OnMouseMove(HWND hWnd, POINTS pt, WPARAM wParam);
void ModernUI_OnLButtonUp(HWND hWnd, POINTS pt);
BOOL ModernUI_OnRButtonDown(HWND hWnd, POINTS pt);

// Refresh hooks called from the engine callback translation units:
//  - UpdateTransport: seek position, time labels, play/pause glyph, volume,
//    status bar text
//  - PlaylistChanged: the playlist's row *count* changed (add/remove/empty) -
//    full ListView_DeleteAllItems + re-insert
//  - ItemChanged: one already-listed item's fields changed (e.g. a tag field
//    just got filled in) - only that row's text is updated. A no-op if the
//    item isn't in the ListView yet, which is the common case: background
//    tag reads run on an item before it's linked into the visible playlist,
//    so every field set during that stage is a no-op here rather than a
//    full rebuild.
//  - ActiveChanged: move the playing marker / selection to the active track
//  - SetBatch: suppress ListView rebuilds during bulk playlist operations
void ModernUI_UpdateTransport(void);
void ModernUI_PlaylistChanged(void);
void ModernUI_ItemChanged(CP_HPLAYLISTITEM hItem);
void ModernUI_ActiveChanged(void);
void ModernUI_SetBatch(BOOL bLock);

// Called from CPI_Player_cb_OnStreamInfo so the status bar format section
// ("FLAC | 1697 kbps | 44.1 kHz | Stereo") has real data.
void ModernUI_OnStreamInfo(unsigned int iBitRate_Kbs, unsigned int iFreq_Hz, unsigned int iBitsPerSample, BOOL bStereo);

#endif
////////////////////////////////////////////////////////////////////////////////
