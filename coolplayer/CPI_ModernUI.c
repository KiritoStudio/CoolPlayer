/*
 * CoolPlayer - foobar2000-style native UI for the main window.
 *
 * Implementation notes:
 *  - Everything is a plain Win32 common control hosted on one child panel:
 *    transport buttons, a seek trackbar with elapsed/total labels, a volume
 *    trackbar, a report-mode ListView playlist and a status bar. The OS
 *    draws all of it (fonts, theming, anti-aliasing) - nothing is
 *    custom-painted and there is no hand-rolled hit-testing.
 *  - The panel has its own window class and wndproc, so WM_COMMAND,
 *    WM_HSCROLL and WM_NOTIFY from the controls never touch main.c.
 *  - Engine integration is unchanged: CPL_/CPLI_ for the playlist,
 *    CPI_Player__Xxx + main_play_control for transport, and the callback
 *    translation units call the ModernUI_UpdateTransport, PlaylistChanged
 *    and ActiveChanged hooks.
 */
////////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "globals.h"
#include "CPI_Player.h"
#include "CPI_Playlist.h"
#include "CPI_PlaylistItem.h"
#include "CPI_ModernUI.h"
#include <commctrl.h>

////////////////////////////////////////////////////////////////////////////////
// Control ids (panel-local)

#define IDC_FOO_STOP     101
#define IDC_FOO_PREV     102
#define IDC_FOO_PLAY     103
#define IDC_FOO_NEXT     104
#define IDC_FOO_ELAPSED  105
#define IDC_FOO_TOTAL    106
#define IDC_FOO_SEEK     107
#define IDC_FOO_VOL      108
#define IDC_FOO_LIST     109
#define IDC_FOO_STATUS   110

// Context menu command ids (TrackPopupMenu TPM_RETURNCMD dispatch)
#define MENUCMD_PLAY         1
#define MENUCMD_PLAYNEXT     2
#define MENUCMD_COPYINFO     3
#define MENUCMD_EXPLORER     4
#define MENUCMD_REMOVE       5
#define MENUCMD_REMOVEFILE   6

// Posted back to ourselves from LVN_KEYDOWN instead of acting inline: any
// handler that can delete/rebuild the ListView's rows (CPL_RemoveItem,
// CPL_PlayItem - both end up calling ModernUI_PlaylistChanged ->
// RebuildPlaylistRows, i.e. ListView_DeleteAllItems + re-insert) must not
// run while still inside a notification *from that same ListView* - doing
// so corrupts comctl32's internal state and crashes a few messages later.
// Posting defers the actual work until after LVN_KEYDOWN has returned.
#define WMAPP_REMOVE_SELECTED    (WM_APP + 1)
#define WMAPP_TRANSPORT_TOGGLE   (WM_APP + 2)
#define WMAPP_TRANSPORT_PLAY     (WM_APP + 3)

#define FOO_TOOLBAR_H    32
#define FOO_SEEK_GRAN    1000

static const char* FOOCLASS_PANEL = "CoolPlayerFooPanel";

////////////////////////////////////////////////////////////////////////////////
// State

static HWND g_hWnd = NULL;         // main window
static HWND g_hPanel = NULL;       // hosts every control
static HWND g_hBtnStop = NULL;
static HWND g_hBtnPrev = NULL;
static HWND g_hBtnPlay = NULL;
static HWND g_hBtnNext = NULL;
static HWND g_hLblElapsed = NULL;
static HWND g_hLblTotal = NULL;
static HWND g_hSeek = NULL;
static HWND g_hVol = NULL;
static HWND g_hList = NULL;
static HWND g_hStatus = NULL;
static WNDPROC g_pfnStatusOrigProc = NULL;
static WNDPROC g_pfnTrackbarOrigProc = NULL;
static HFONT g_hFontUI = NULL;

static BOOL g_bSeekDragging = FALSE;
static int g_iBatchDepth = 0;
static BOOL g_bBatchDirty = FALSE;
static int g_iSortColumn = -1;
static BOOL g_bSortDesc = FALSE;

static unsigned int g_fmtBitrateKbs = 0;
static unsigned int g_fmtFreqHz = 0;
static BOOL g_fmtStereo = TRUE;
static BOOL g_fmtValid = FALSE;

// Space/Enter re-issue CPL_PlayItem, which round-trips through the engine
// thread's CPTM_OPENFILE (full close+reopen of the codec). That path only
// de-dupes *queued* opens against each other, not against the STOP that
// CPL_PlayItem sends first - key-repeat (holding the key down) can fire
// this far faster than a human clicking the Play button ever would, faster
// than the engine thread was ever exercised at. Debounce at the source
// instead of trusting the decades-old engine code to absorb the flood.
static DWORD g_dwLastTransportKeyTick = 0;
#define TRANSPORT_KEY_DEBOUNCE_MS 350

////////////////////////////////////////////////////////////////////////////////
// Forward declarations

static LRESULT CALLBACK FooPanelProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK StatusBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TrackbarWheelSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static BOOL RouteMouseWheel(POINT pt, WPARAM wParam, LPARAM lParam);
static void CreateControls(HWND hPanel);
static void LayoutControls(void);
static void RebuildPlaylistRows(void);
static void UpdatePlayingMarker(void);
static void ShowRowContextMenu(int idx, int xScreen, int yScreen);
static void CopyItemInfoToClipboard(CP_HPLAYLISTITEM hItem);
static void ShowItemInExplorer(CP_HPLAYLISTITEM hItem);
static void PlayRow(int idx);
static void OnColumnClick(int iColumn);
static CP_HPLAYLISTITEM GetItemAtIndex(int idx);
static int GetActiveIndex(void);
static int GetIndexOfItem(CP_HPLAYLISTITEM hTarget);
static void ModernUI_SelectAllRows(void);
static void ModernUI_RemoveSelectedRows(void);
static void ComputeTrianglePoints(const RECT* prc, int iDir, POINT pts[3]);
static void DrawIconTriangle(HDC hdc, const RECT* prc, int iDir);
static void DrawTransportIcon(HDC hdc, const RECT* prc, int iCtlID);
static void FormatSecs(char* buf, unsigned long secs);

////////////////////////////////////////////////////////////////////////////////
// Playlist helpers - always walk the live CPL_ list, no separate UI-side cache

static CP_HPLAYLISTITEM GetItemAtIndex(int idx)
{
	CP_HPLAYLISTITEM h;
	int i;

	if (idx < 0)
		return NULL;

	h = CPL_GetFirstItem(globals.m_hPlaylist);

	for (i = 0; i < idx && h; i++)
		h = CPLI_Next(h);

	return h;
}

static int GetActiveIndex(void)
{
	CP_HPLAYLISTITEM hActive = CPL_GetActiveItem(globals.m_hPlaylist);
	CP_HPLAYLISTITEM h;
	int i;

	if (!hActive)
		return -1;

	for (h = CPL_GetFirstItem(globals.m_hPlaylist), i = 0; h; h = CPLI_Next(h), i++)
	{
		if (h == hActive)
			return i;
	}

	return -1;
}

static int GetIndexOfItem(CP_HPLAYLISTITEM hTarget)
{
	CP_HPLAYLISTITEM h;
	int i;

	if (!hTarget)
		return -1;

	for (h = CPL_GetFirstItem(globals.m_hPlaylist), i = 0; h; h = CPLI_Next(h), i++)
	{
		if (h == hTarget)
			return i;
	}

	return -1;
}

static void FormatSecs(char* buf, unsigned long secs)
{
	wsprintf(buf, "%d:%02d", (int)(secs / 60), (int)(secs % 60));
}

// GDI-drawn transport icons: vector shapes instead of font glyphs, so they
// render identically regardless of what the Segoe UI build on this device
// supports.
static void ComputeTrianglePoints(const RECT* prc, int iDir, POINT pts[3])
{
	int cx = (prc->left + prc->right) / 2;
	int cy = (prc->top + prc->bottom) / 2;
	int halfW = (prc->right - prc->left) / 2;
	int halfH = (prc->bottom - prc->top) / 2;

	if (halfW < 1) halfW = 1;
	if (halfH < 1) halfH = 1;

	if (iDir > 0)
	{
		pts[0].x = cx - halfW; pts[0].y = cy - halfH;
		pts[1].x = cx - halfW; pts[1].y = cy + halfH;
		pts[2].x = cx + halfW; pts[2].y = cy;
	}
	else
	{
		pts[0].x = cx + halfW; pts[0].y = cy - halfH;
		pts[1].x = cx + halfW; pts[1].y = cy + halfH;
		pts[2].x = cx - halfW; pts[2].y = cy;
	}
}

static void DrawIconTriangle(HDC hdc, const RECT* prc, int iDir)
{
	POINT pts[3];

	ComputeTrianglePoints(prc, iDir, pts);
	Polygon(hdc, pts, 3);
}

static void DrawTransportIcon(HDC hdc, const RECT* prc, int iCtlID)
{
	RECT rc = *prc;
	int barW = (rc.right - rc.left) / 5;

	switch (iCtlID)
	{
		case IDC_FOO_STOP:
			Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
			break;

		case IDC_FOO_PREV:
		{
			RECT rcTri = rc;
			rcTri.left += barW * 2;
			Rectangle(hdc, rc.left, rc.top, rc.left + barW, rc.bottom);
			DrawIconTriangle(hdc, &rcTri, -1);
			break;
		}

		case IDC_FOO_NEXT:
		{
			RECT rcTri = rc;
			rcTri.right -= barW * 2;
			Rectangle(hdc, rc.right - barW, rc.top, rc.right, rc.bottom);
			DrawIconTriangle(hdc, &rcTri, 1);
			break;
		}

		case IDC_FOO_PLAY:

			if (globals.m_enPlayerState == cppsPlaying)
			{
				int pauseBarW = ((rc.right - rc.left) - 2) / 2;
				Rectangle(hdc, rc.left, rc.top, rc.left + pauseBarW, rc.bottom);
				Rectangle(hdc, rc.right - pauseBarW, rc.top, rc.right, rc.bottom);
			}
			else
				DrawIconTriangle(hdc, &rc, 1);

			break;
	}
}

static void ModernUI_SelectAllRows(void)
{
	if (!g_hList)
		return;

	ListView_SetItemState(g_hList, -1, LVIS_SELECTED, LVIS_SELECTED);
}

static void ModernUI_RemoveSelectedRows(void)
{
	int iSelCount;
	CP_HPLAYLISTITEM* ahItems;
	int iCount = 0;
	int idx;

	if (!g_hList)
		return;

	iSelCount = ListView_GetSelectedCount(g_hList);

	if (iSelCount <= 0)
		return;

	ahItems = (CP_HPLAYLISTITEM*)malloc(sizeof(CP_HPLAYLISTITEM) * iSelCount);

	if (!ahItems)
		return;

	idx = -1;

	while ((idx = ListView_GetNextItem(g_hList, idx, LVNI_SELECTED)) != -1 && iCount < iSelCount)
	{
		CP_HPLAYLISTITEM h = GetItemAtIndex(idx);

		if (h)
			ahItems[iCount++] = h;
	}

	ModernUI_SetBatch(TRUE);

	for (idx = 0; idx < iCount; idx++)
		CPL_RemoveItem(globals.m_hPlaylist, ahItems[idx]);

	ModernUI_SetBatch(FALSE);

	free(ahItems);
}

////////////////////////////////////////////////////////////////////////////////
// Context-menu actions (unchanged logic from the previous UI)

static void CopyItemInfoToClipboard(CP_HPLAYLISTITEM hItem)
{
	char buf[640];
	const char* pTitle;
	const char* pArtist;
	const char* pLen;
	HGLOBAL hMem;
	char* pMem;
	int len;

	pTitle = CPLI_GetTrackName(hItem);

	if (!pTitle || !pTitle[0])
		pTitle = CPLI_GetFilename(hItem);

	pArtist = CPLI_GetArtist(hItem);
	pLen = CPLI_GetTrackLength_AsText(hItem);

	lstrcpyn(buf, (pTitle && pTitle[0]) ? pTitle : "(Unknown Track)", 256);

	if (pArtist && pArtist[0])
	{
		lstrcat(buf, " - ");
		lstrcpyn(buf + lstrlen(buf), pArtist, 256);
	}

	if (pLen && pLen[0])
	{
		lstrcat(buf, " (");
		lstrcpyn(buf + lstrlen(buf), pLen, 32);
		lstrcat(buf, ")");
	}

	len = lstrlen(buf);

	if (!OpenClipboard(g_hWnd))
		return;

	EmptyClipboard();
	hMem = GlobalAlloc(GMEM_MOVEABLE, len + 1);

	if (hMem)
	{
		pMem = (char*)GlobalLock(hMem);

		if (pMem)
		{
			memcpy(pMem, buf, len + 1);
			GlobalUnlock(hMem);
			SetClipboardData(CF_TEXT, hMem);
		}
		else
			GlobalFree(hMem);
	}

	CloseClipboard();
}

static void ShowItemInExplorer(CP_HPLAYLISTITEM hItem)
{
	const char* pPath = CPLI_GetPath(hItem);
	char args[MAX_PATH + 16];

	if (!pPath || !pPath[0])
		return;

	wsprintf(args, "/select,\"%s\"", pPath);
	ShellExecute(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static void PlayRow(int idx)
{
	CP_HPLAYLISTITEM h = GetItemAtIndex(idx);

	if (h)
	{
		CPL_SetActiveItem(globals.m_hPlaylist, h);
		CPL_PlayItem(globals.m_hPlaylist, TRUE, pmCurrentItem);
	}
}

static void ShowRowContextMenu(int idx, int xScreen, int yScreen)
{
	CP_HPLAYLISTITEM hItem = GetItemAtIndex(idx);
	HMENU hMenu;
	int cmd;

	if (!hItem)
		return;

	hMenu = CreatePopupMenu();

	if (!hMenu)
		return;

	AppendMenu(hMenu, MF_STRING, MENUCMD_PLAY, "Play");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, MENUCMD_PLAYNEXT, "Play Next");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, MENUCMD_COPYINFO, "Copy Info to Clipboard");
	AppendMenu(hMenu, MF_STRING, MENUCMD_EXPLORER, "Show in Explorer");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, MENUCMD_REMOVE, "Remove");
	AppendMenu(hMenu, MF_STRING, MENUCMD_REMOVEFILE, "Remove with File");

	cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
						 xScreen, yScreen, 0, g_hPanel, NULL);
	DestroyMenu(hMenu);

	switch (cmd)
	{
		case MENUCMD_PLAY:
			PlayRow(idx);
			break;

		case MENUCMD_PLAYNEXT:
			CPL_Stack_PlayNext(globals.m_hPlaylist, hItem);
			break;

		case MENUCMD_COPYINFO:
			CopyItemInfoToClipboard(hItem);
			break;

		case MENUCMD_EXPLORER:
			ShowItemInExplorer(hItem);
			break;

		case MENUCMD_REMOVE:
			CPL_RemoveItem(globals.m_hPlaylist, hItem);
			break;

		case MENUCMD_REMOVEFILE:
		{
			// Destructive: confirm, snapshot the path BEFORE the item is
			// freed by the removal, then delete the file from disk.
			char path[MAX_PATH];
			char msg[MAX_PATH + 64];
			const char* pPath = CPLI_GetPath(hItem);

			if (!pPath || !pPath[0])
				break;

			lstrcpyn(path, pPath, MAX_PATH);
			wsprintf(msg, "Delete this file from disk?\n\n%s", path);

			if (MessageBox(g_hWnd, msg, "Remove with File", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
			{
				CPL_RemoveItem(globals.m_hPlaylist, hItem);

				if (!DeleteFile(path))
					MessageBox(g_hWnd, "The file could not be deleted.\nIt may be in use.",
							   "Remove with File", MB_OK | MB_ICONINFORMATION);
			}

			break;
		}

		default:
			break;
	}
}

////////////////////////////////////////////////////////////////////////////////
// ListView population

static void RebuildPlaylistRows(void)
{
	CP_HPLAYLISTITEM h;
	CP_HPLAYLISTITEM hActive;
	LVITEM lvi;
	int idx;
	int iActive = -1;

	if (!g_hList)
		return;

	if (g_iBatchDepth > 0)
	{
		g_bBatchDirty = TRUE;
		return;
	}

	SendMessage(g_hList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(g_hList);

	hActive = CPL_GetActiveItem(globals.m_hPlaylist);

	for (h = CPL_GetFirstItem(globals.m_hPlaylist), idx = 0; h; h = CPLI_Next(h), idx++)
	{
		const char* pArtist = CPLI_GetArtist(h);
		const char* pAlbum = CPLI_GetAlbum(h);
		const char* pTitle = CPLI_GetTrackName(h);
		const char* pLen = CPLI_GetTrackLength_AsText(h);

		if (!pTitle || !pTitle[0])
			pTitle = CPLI_GetFilename(h);

		memset(&lvi, 0, sizeof(lvi));
		lvi.mask = LVIF_TEXT;
		lvi.iItem = idx;
		lvi.iSubItem = 0;
		lvi.pszText = (h == hActive) ? ">" : "";
		ListView_InsertItem(g_hList, &lvi);

		ListView_SetItemText(g_hList, idx, 1, (LPSTR)((pArtist && pArtist[0]) ? pArtist : "?"));
		ListView_SetItemText(g_hList, idx, 2, (LPSTR)((pAlbum && pAlbum[0]) ? pAlbum : "?"));
		ListView_SetItemText(g_hList, idx, 3, (LPSTR)((pTitle && pTitle[0]) ? pTitle : "(Unknown Track)"));
		ListView_SetItemText(g_hList, idx, 4, (LPSTR)((pLen && pLen[0]) ? pLen : "?"));

		if (h == hActive)
			iActive = idx;
	}

	SendMessage(g_hList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(g_hList, NULL, TRUE);

	if (iActive >= 0)
		ListView_EnsureVisible(g_hList, iActive, FALSE);
}

static void UpdatePlayingMarker(void)
{
	int count;
	int idx;
	int iActive;

	if (!g_hList)
		return;

	count = ListView_GetItemCount(g_hList);
	iActive = GetActiveIndex();

	for (idx = 0; idx < count; idx++)
		ListView_SetItemText(g_hList, idx, 0, (LPSTR)((idx == iActive) ? ">" : ""));

	if (iActive >= 0)
		ListView_EnsureVisible(g_hList, iActive, FALSE);
}

static void OnColumnClick(int iColumn)
{
	CPe_PlayItemSortElement enElement;

	switch (iColumn)
	{
		case 1: enElement = piseArtist;    break;
		case 2: enElement = piseAlbum;     break;
		case 3: enElement = piseTrackName; break;
		case 4: enElement = piseLength;    break;
		default: return;
	}

	if (g_iSortColumn == iColumn)
		g_bSortDesc = !g_bSortDesc;
	else
	{
		g_iSortColumn = iColumn;
		g_bSortDesc = FALSE;
	}

	CPL_SortList(globals.m_hPlaylist, enElement, g_bSortDesc);
	RebuildPlaylistRows();
}

////////////////////////////////////////////////////////////////////////////////
// Status bar flicker fix
//
// msctls_statusbar32 (STATUSCLASSNAME) erases its whole client area as a
// separate step before redrawing text on every SB_SETTEXT, and on this
// hardware's GDI stack that shows up as a visible flash even though it's
// only one update per second (already cut down from three - see
// main_draw_bitrate/frequency in main.c). Just suppressing WM_ERASEBKGND
// was tried first and made things worse: without the erase, text from the
// previous frame that isn't covered by the new text's glyph footprint (a
// shorter string, or "Stopped." vs a much longer "Playing | ..." line)
// stays on screen and visibly ghosts/overlaps the new text.
//
// The correct fix is real double buffering: let the control's own default
// paint logic erase-and-draw as normal, just redirected into an off-screen
// memory bitmap first, then copy the finished result to the screen in one
// BitBlt. The user only ever sees a complete frame - never the intermediate
// erased-but-not-yet-texted state that caused the flash, and never stale
// pixels the new text didn't cover. WM_PRINTCLIENT is the standard way to
// ask a common control to render its current appearance into an arbitrary
// HDC; comctl32's status bar has supported it since Windows 2000/XP.
static LRESULT CALLBACK StatusBarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_PAINT)
	{
		PAINTSTRUCT ps;
		HDC hdcReal;
		HDC hdcMem;
		HBITMAP hbmMem, hbmOld;
		RECT rc;

		hdcReal = BeginPaint(hWnd, &ps);
		GetClientRect(hWnd, &rc);

		hdcMem = CreateCompatibleDC(hdcReal);
		hbmMem = CreateCompatibleBitmap(hdcReal, rc.right, rc.bottom);
		hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

		CallWindowProc(g_pfnStatusOrigProc, hWnd, WM_ERASEBKGND, (WPARAM)hdcMem, 0);
		CallWindowProc(g_pfnStatusOrigProc, hWnd, WM_PRINTCLIENT, (WPARAM)hdcMem, PRF_CLIENT);

		BitBlt(hdcReal, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);

		SelectObject(hdcMem, hbmOld);
		DeleteObject(hbmMem);
		DeleteDC(hdcMem);

		EndPaint(hWnd, &ps);
		return 0;
	}

	if (uMsg == WM_ERASEBKGND)
		return 1;

	return CallWindowProc(g_pfnStatusOrigProc, hWnd, uMsg, wParam, lParam);
}

// WM_MOUSEWHEEL is delivered to whichever window currently has keyboard
// focus, not to whatever the mouse happens to be over - unlike most other
// mouse messages, and unlike what a user would reasonably expect ("scroll
// the thing the cursor is on"). Most of the time nothing has taken focus
// away from the main window, so every wheel notch anywhere in the window
// (the playlist included) reaches main.c's own WM_MOUSEWHEEL handler, which
// unconditionally treats it as a volume change - that's what actually moves
// the volume slider (main.c adjusts globals.m_iVolume and the trackbar's
// on-screen thumb just gets resynced to it on the next transport update,
// it's not the trackbar reacting to the wheel itself). Clicking a trackbar
// first can also leave *it* with focus instead, which routes the message
// differently but has the same "wrong control eats every wheel notch"
// symptom - see TrackbarWheelSubclassProc below for that path.
//
// Routes a wheel notch to whichever control the cursor is actually over:
// the ListView (this build's comctl32 doesn't scroll it on WM_MOUSEWHEEL by
// itself - confirmed on-device: sending the message directly did nothing -
// so it's driven explicitly via ListView_Scroll instead of just forwarding
// and hoping) or one of the trackbars. Returns FALSE if the cursor isn't
// over anything this function wants to claim, leaving the caller free to
// fall back to whatever it did before this existed.
static BOOL RouteMouseWheel(POINT pt, WPARAM wParam, LPARAM lParam)
{
	RECT rc;

	if (g_hList && GetWindowRect(g_hList, &rc) && PtInRect(&rc, pt))
	{
		short zDelta = (short)HIWORD(wParam);
		RECT rcItem;
		int itemHeight = 18;  // fallback if the list has no rows to measure yet

		// LVM_GETITEMSPACING only applies to icon/tile views, not LVS_REPORT
		// (the one this list uses) - measuring an actual row's rect is the
		// correct way to get a report-view row height.
		if (ListView_GetItemRect(g_hList, 0, &rcItem, LVIR_BOUNDS))
			itemHeight = rcItem.bottom - rcItem.top;

		// zDelta isn't guaranteed to be a clean multiple of WHEEL_DELTA
		// (precision touchpads/trackballs routinely report smaller deltas
		// per notch) - dividing first (zDelta / WHEEL_DELTA) truncates to 0
		// for any single notch below that, silently turning every scroll
		// into a no-op. Just use the sign and move a fixed 3 rows per call.
		ListView_Scroll(g_hList, 0, (zDelta < 0 ? 1 : -1) * 3 * itemHeight);
		return TRUE;
	}

	if (g_hVol && GetWindowRect(g_hVol, &rc) && PtInRect(&rc, pt))
	{
		CallWindowProc(g_pfnTrackbarOrigProc, g_hVol, WM_MOUSEWHEEL, wParam, lParam);
		return TRUE;
	}

	if (g_hSeek && GetWindowRect(g_hSeek, &rc) && PtInRect(&rc, pt))
	{
		CallWindowProc(g_pfnTrackbarOrigProc, g_hSeek, WM_MOUSEWHEEL, wParam, lParam);
		return TRUE;
	}

	return FALSE;
}

// Both trackbars are subclassed with this proc so a wheel notch that only
// reached one of them because it had stale focus (see RouteMouseWheel's
// comment above) gets redirected to whatever the cursor is really over,
// instead of always moving that trackbar's thumb.
static LRESULT CALLBACK TrackbarWheelSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_MOUSEWHEEL)
	{
		POINT pt;
		RECT rc;

		pt.x = (short)LOWORD(lParam);
		pt.y = (short)HIWORD(lParam);
		GetWindowRect(hWnd, &rc);

		if (!PtInRect(&rc, pt))
		{
			RouteMouseWheel(pt, wParam, lParam);
			return 0;
		}
	}

	return CallWindowProc(g_pfnTrackbarOrigProc, hWnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Control creation and layout

static void CreateControls(HWND hPanel)
{
	HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hPanel, GWLP_HINSTANCE);
	LVCOLUMN lvc;
	NONCLIENTMETRICS ncm;

	// Owner-drawn: icons are GDI vector shapes (see DrawTransportIcon), not
	// font glyphs, so they render identically regardless of what the Segoe
	// UI build on this device supports.
	g_hBtnStop = CreateWindowEx(0, "BUTTON", "",
								WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
								0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_STOP, hInst, NULL);
	g_hBtnPrev = CreateWindowEx(0, "BUTTON", "",
								WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
								0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_PREV, hInst, NULL);
	g_hBtnPlay = CreateWindowEx(0, "BUTTON", "",
								WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
								0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_PLAY, hInst, NULL);
	g_hBtnNext = CreateWindowEx(0, "BUTTON", "",
								WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
								0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_NEXT, hInst, NULL);

	g_hLblElapsed = CreateWindowEx(0, "STATIC", "0:00",
								   WS_CHILD | WS_VISIBLE | SS_CENTER,
								   0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_ELAPSED, hInst, NULL);
	g_hLblTotal = CreateWindowEx(0, "STATIC", "0:00",
								 WS_CHILD | WS_VISIBLE | SS_CENTER,
								 0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_TOTAL, hInst, NULL);

	g_hSeek = CreateWindowEx(0, TRACKBAR_CLASS, "",
							 WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
							 0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_SEEK, hInst, NULL);
	SendMessage(g_hSeek, TBM_SETRANGE, FALSE, MAKELPARAM(0, FOO_SEEK_GRAN));
	SendMessage(g_hSeek, TBM_SETPAGESIZE, 0, FOO_SEEK_GRAN / 20);

	g_hVol = CreateWindowEx(0, TRACKBAR_CLASS, "",
							WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
							0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_VOL, hInst, NULL);
	SendMessage(g_hVol, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
	SendMessage(g_hVol, TBM_SETPAGESIZE, 0, 5);
	SendMessage(g_hVol, TBM_SETPOS, TRUE, globals.m_iVolume);

	// Both trackbars share one subclass proc - comctl32 backs every
	// TRACKBAR_CLASS instance with the same wndproc, so one saved pointer
	// works for calling either one's original behaviour back.
	g_pfnTrackbarOrigProc = (WNDPROC)SetWindowLongPtr(g_hSeek, GWLP_WNDPROC, (LONG_PTR)TrackbarWheelSubclassProc);
	SetWindowLongPtr(g_hVol, GWLP_WNDPROC, (LONG_PTR)TrackbarWheelSubclassProc);

	g_hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, "",
							 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT
							 | LVS_SHOWSELALWAYS,
							 0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_LIST, hInst, NULL);
	ListView_SetExtendedListViewStyle(g_hList,
									  LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);

	memset(&lvc, 0, sizeof(lvc));
	lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

	lvc.fmt = LVCFMT_LEFT;  lvc.cx = 26;  lvc.pszText = (LPSTR)"";       ListView_InsertColumn(g_hList, 0, &lvc);
	lvc.fmt = LVCFMT_LEFT;  lvc.cx = 150; lvc.pszText = (LPSTR)"Artist"; ListView_InsertColumn(g_hList, 1, &lvc);
	lvc.fmt = LVCFMT_LEFT;  lvc.cx = 150; lvc.pszText = (LPSTR)"Album";  ListView_InsertColumn(g_hList, 2, &lvc);
	lvc.fmt = LVCFMT_LEFT;  lvc.cx = 240; lvc.pszText = (LPSTR)"Title";  ListView_InsertColumn(g_hList, 3, &lvc);
	lvc.fmt = LVCFMT_RIGHT; lvc.cx = 64;  lvc.pszText = (LPSTR)"Length"; ListView_InsertColumn(g_hList, 4, &lvc);

	g_hStatus = CreateWindowEx(0, STATUSCLASSNAME, "Stopped.",
							   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
							   0, 0, 10, 10, hPanel, (HMENU)(INT_PTR)IDC_FOO_STATUS, hInst, NULL);

	g_pfnStatusOrigProc = (WNDPROC)SetWindowLongPtr(g_hStatus, GWLP_WNDPROC, (LONG_PTR)StatusBarSubclassProc);

	// The standard message font (Segoe UI 9pt on Win8.x) for every control
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);

	if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
		g_hFontUI = CreateFontIndirect(&ncm.lfMessageFont);

	if (g_hFontUI)
	{
		SendMessage(g_hLblElapsed, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
		SendMessage(g_hLblTotal, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
		SendMessage(g_hList, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
		SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
	}
}

static void LayoutControls(void)
{
	RECT rc, rcStatus;
	int w, h, x;
	int statusH;
	int seekLeft, seekRight;

	if (!g_hPanel)
		return;

	GetClientRect(g_hPanel, &rc);
	w = rc.right;
	h = rc.bottom;

	if (w < 420)
		w = 420;

	// Status bar sizes itself; measure it for the ListView height
	SendMessage(g_hStatus, WM_SIZE, 0, 0);
	GetWindowRect(g_hStatus, &rcStatus);
	statusH = rcStatus.bottom - rcStatus.top;

	// Toolbar row
	x = 6;
	MoveWindow(g_hBtnStop, x, 5, 28, 22, TRUE); x += 30;
	MoveWindow(g_hBtnPrev, x, 5, 28, 22, TRUE); x += 30;
	MoveWindow(g_hBtnPlay, x, 5, 32, 22, TRUE); x += 34;
	MoveWindow(g_hBtnNext, x, 5, 28, 22, TRUE); x += 34;

	MoveWindow(g_hLblElapsed, x, 9, 40, 15, TRUE);
	seekLeft = x + 42;

	// Volume on the far right, total-time label just before it
	MoveWindow(g_hVol, w - 106, 5, 100, 22, TRUE);
	MoveWindow(g_hLblTotal, w - 106 - 46, 9, 40, 15, TRUE);
	seekRight = w - 106 - 50;

	if (seekRight - seekLeft < 60)
		seekRight = seekLeft + 60;

	MoveWindow(g_hSeek, seekLeft, 5, seekRight - seekLeft, 22, TRUE);

	// Playlist fills the rest
	{
		int listH = h - FOO_TOOLBAR_H - statusH;

		if (listH < 40)
			listH = 40;

		MoveWindow(g_hList, 0, FOO_TOOLBAR_H, w, listH, TRUE);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Panel wndproc - all control traffic is handled here, not in main.c

static LRESULT CALLBACK FooPanelProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_SIZE:
			LayoutControls();
			return 0;

		case WM_MOUSEWHEEL:
		{
			POINT pt;

			pt.x = (short)LOWORD(lParam);
			pt.y = (short)HIWORD(lParam);
			RouteMouseWheel(pt, wParam, lParam);
			return 0;
		}

		case WMAPP_REMOVE_SELECTED:
			ModernUI_RemoveSelectedRows();
			return 0;

		case WMAPP_TRANSPORT_TOGGLE:
			if (globals.m_enPlayerState == cppsPlaying)
				main_play_control(ID_PAUSE, g_hWnd);
			else
				main_play_control(ID_PLAY, g_hWnd);
			return 0;

		case WMAPP_TRANSPORT_PLAY:
			main_play_control(ID_PLAY, g_hWnd);
			return 0;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_FOO_STOP:
					main_play_control(ID_STOP, g_hWnd);
					break;

				case IDC_FOO_PREV:
					main_play_control(ID_PREVIOUS, g_hWnd);
					break;

				case IDC_FOO_PLAY:
					if (globals.m_enPlayerState == cppsPlaying)
						main_play_control(ID_PAUSE, g_hWnd);
					else
						main_play_control(ID_PLAY, g_hWnd);
					break;

				case IDC_FOO_NEXT:
					main_play_control(ID_NEXT, g_hWnd);
					break;

				default:
					break;
			}

			// Give focus back to the list so keyboard navigation keeps working
			if (g_hList)
				SetFocus(g_hList);

			return 0;

		case WM_HSCROLL:
		{
			HWND hCtl = (HWND)lParam;

			if (hCtl == g_hSeek)
			{
				const int code = LOWORD(wParam);
				int pos;

				if (code == TB_THUMBTRACK)
				{
					// Update the elapsed label live while dragging
					char buf[16];
					g_bSeekDragging = TRUE;
					pos = (int)SendMessage(g_hSeek, TBM_GETPOS, 0, 0);
					FormatSecs(buf, (unsigned long)(globals.main_long_track_duration * pos / FOO_SEEK_GRAN));
					SetWindowText(g_hLblElapsed, buf);
				}
				else if (code == TB_THUMBPOSITION || code == TB_ENDTRACK
						 || code == TB_PAGEUP || code == TB_PAGEDOWN
						 || code == TB_LINEUP || code == TB_LINEDOWN)
				{
					g_bSeekDragging = FALSE;
					pos = (int)SendMessage(g_hSeek, TBM_GETPOS, 0, 0);

					globals.main_int_track_position = pos * MODERNUI_SEEK_RANGE / FOO_SEEK_GRAN;

					if (globals.m_hPlayer)
						CPI_Player__Seek(globals.m_hPlayer, globals.main_int_track_position, MODERNUI_SEEK_RANGE);
				}
			}

			else if (hCtl == g_hVol)
			{
				int vol = (int)SendMessage(g_hVol, TBM_GETPOS, 0, 0);

				if (vol < 0) vol = 0;
				if (vol > 100) vol = 100;

				globals.m_iVolume = vol;

				if (globals.m_hPlayer)
					CPI_Player__SetVolume(globals.m_hPlayer, vol);
			}

			return 0;
		}

		case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;

			if (pdis->CtlID == IDC_FOO_STOP || pdis->CtlID == IDC_FOO_PREV
				|| pdis->CtlID == IDC_FOO_PLAY || pdis->CtlID == IDC_FOO_NEXT)
			{
				RECT rcIcon = pdis->rcItem;
				UINT uFrameState = DFCS_BUTTONPUSH | ((pdis->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0);

				DrawFrameControl(pdis->hDC, &pdis->rcItem, DFC_BUTTON, uFrameState);

				InflateRect(&rcIcon, -9, -6);

				if (pdis->itemState & ODS_SELECTED)
					OffsetRect(&rcIcon, 1, 1);

				SelectObject(pdis->hDC, GetStockObject(BLACK_BRUSH));
				SelectObject(pdis->hDC, GetStockObject(NULL_PEN));
				DrawTransportIcon(pdis->hDC, &rcIcon, pdis->CtlID);

				return TRUE;
			}

			break;
		}

		case WM_NOTIFY:
		{
			NMHDR* pHdr = (NMHDR*)lParam;

			if (pHdr && pHdr->hwndFrom == g_hList)
			{
				switch (pHdr->code)
				{
					case LVN_ITEMACTIVATE:  // double-click or Enter
					{
						NMITEMACTIVATE* pAct = (NMITEMACTIVATE*)lParam;

						if (pAct->iItem >= 0)
							PlayRow(pAct->iItem);

						return 0;
					}

					case NM_RCLICK:
					{
						NMITEMACTIVATE* pAct = (NMITEMACTIVATE*)lParam;

						if (pAct->iItem >= 0)
						{
							POINT pt = pAct->ptAction;
							ClientToScreen(g_hList, &pt);
							ShowRowContextMenu(pAct->iItem, pt.x, pt.y);
						}
						else
						{
							// Empty area: the legacy application menu
							// (Options / Open / Exit and friends)
							POINT pt;
							GetCursorPos(&pt);
							main_menuproc(g_hWnd, &pt);
						}

						return 0;
					}

					case LVN_COLUMNCLICK:
					{
						NMLISTVIEW* pLv = (NMLISTVIEW*)lParam;
						OnColumnClick(pLv->iSubItem);
						return 0;
					}

					case LVN_KEYDOWN:
					{
						NMLVKEYDOWN* pKey = (NMLVKEYDOWN*)lParam;

						if (pKey->wVKey == VK_DELETE || pKey->wVKey == VK_BACK)
							PostMessage(hWnd, WMAPP_REMOVE_SELECTED, 0, 0);
						else if (pKey->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
							ModernUI_SelectAllRows();  // just flips state bits - safe inline
						else if (pKey->wVKey == VK_SPACE || pKey->wVKey == VK_RETURN)
						{
							DWORD dwNow = GetTickCount();

							if (dwNow - g_dwLastTransportKeyTick >= TRANSPORT_KEY_DEBOUNCE_MS)
							{
								g_dwLastTransportKeyTick = dwNow;
								PostMessage(hWnd, (pKey->wVKey == VK_SPACE) ? WMAPP_TRANSPORT_TOGGLE : WMAPP_TRANSPORT_PLAY, 0, 0);
							}
						}

						return 0;
					}

					default:
						break;
				}
			}

			break;
		}

		case WM_RBUTTONDOWN:
		{
			// Right-click on the toolbar strip: legacy application menu
			POINT pt;
			GetCursorPos(&pt);
			main_menuproc(g_hWnd, &pt);
			return 0;
		}

		case WM_CTLCOLORSTATIC:
			SetBkMode((HDC)wParam, TRANSPARENT);
			return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

		case WM_ERASEBKGND:
		{
			RECT rc;
			GetClientRect(hWnd, &rc);
			FillRect((HDC)wParam, &rc, GetSysColorBrush(COLOR_BTNFACE));
			return 1;
		}

		default:
			break;
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Refresh hooks (called from the engine callback translation units)

void ModernUI_UpdateTransport(void)
{
	char buf[64];
	int pos;

	if (!g_hPanel)
		return;

	// Seek position (unless the user is mid-drag)
	pos = globals.m_bStreaming ? globals.m_iStreamingPortion : globals.main_int_track_position;

	if (pos < 0) pos = 0;
	if (pos > MODERNUI_SEEK_RANGE) pos = MODERNUI_SEEK_RANGE;

	if (!g_bSeekDragging)
	{
		SendMessage(g_hSeek, TBM_SETPOS, TRUE, pos * FOO_SEEK_GRAN / MODERNUI_SEEK_RANGE);

		FormatSecs(buf, globals.main_int_track_total_seconds);
		SetWindowText(g_hLblElapsed, buf);
	}

	FormatSecs(buf, globals.main_long_track_duration);
	SetWindowText(g_hLblTotal, buf);

	// Volume (skip while the user is dragging that slider)
	if (GetCapture() != g_hVol)
		SendMessage(g_hVol, TBM_SETPOS, TRUE, globals.m_iVolume);

	// Play/pause icon (owner-drawn; state read in DrawTransportIcon)
	InvalidateRect(g_hBtnPlay, NULL, TRUE);

	// Status bar, foobar style:
	// "Playing | FLAC | 1015 kbps | 44.1 kHz | Stereo | 0:23 / 3:41 | Title"
	{
		char status[512];
		char elapsed[16], total[16];

		if (globals.m_enPlayerState == cppsPlaying || globals.m_enPlayerState == cppsPaused)
		{
			CP_HPLAYLISTITEM hActive = CPL_GetActiveItem(globals.m_hPlaylist);
			const char* pTitle = NULL;

			if (hActive)
			{
				pTitle = CPLI_GetTrackName(hActive);

				if (!pTitle || !pTitle[0])
					pTitle = CPLI_GetFilename(hActive);
			}

			FormatSecs(elapsed, globals.main_int_track_total_seconds);
			FormatSecs(total, globals.main_long_track_duration);

			lstrcpy(status, (globals.m_enPlayerState == cppsPlaying) ? "Playing" : "Paused");

			if (g_fmtValid)
			{
				char part[96];
				char kbps[32];

				if (g_fmtBitrateKbs > 0)
					wsprintf(kbps, "%d kbps  |  ", g_fmtBitrateKbs);
				else
					kbps[0] = 0;

				wsprintf(part, "  |  %s%d.%d kHz  |  %s", kbps,
						 g_fmtFreqHz / 1000, (g_fmtFreqHz / 100) % 10,
						 g_fmtStereo ? "stereo" : "mono");
				lstrcat(status, part);
			}

			wsprintf(status + lstrlen(status), "  |  %s / %s", elapsed, total);

			if (pTitle && pTitle[0])
			{
				lstrcat(status, "  |  ");
				lstrcpyn(status + lstrlen(status), pTitle, 200);
			}
		}
		else
			lstrcpy(status, "Stopped.");

		SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)status);
	}
}

void ModernUI_PlaylistChanged(void)
{
	RebuildPlaylistRows();
}

void ModernUI_ItemChanged(CP_HPLAYLISTITEM hItem)
{
	int idx;
	const char* pArtist;
	const char* pAlbum;
	const char* pTitle;
	const char* pLen;

	if (!g_hList)
		return;

	idx = GetIndexOfItem(hItem);

	if (idx < 0)
		return;

	pArtist = CPLI_GetArtist(hItem);
	pAlbum = CPLI_GetAlbum(hItem);
	pTitle = CPLI_GetTrackName(hItem);
	pLen = CPLI_GetTrackLength_AsText(hItem);

	if (!pTitle || !pTitle[0])
		pTitle = CPLI_GetFilename(hItem);

	ListView_SetItemText(g_hList, idx, 1, (LPSTR)((pArtist && pArtist[0]) ? pArtist : "?"));
	ListView_SetItemText(g_hList, idx, 2, (LPSTR)((pAlbum && pAlbum[0]) ? pAlbum : "?"));
	ListView_SetItemText(g_hList, idx, 3, (LPSTR)((pTitle && pTitle[0]) ? pTitle : "(Unknown Track)"));
	ListView_SetItemText(g_hList, idx, 4, (LPSTR)((pLen && pLen[0]) ? pLen : "?"));
}

void ModernUI_ActiveChanged(void)
{
	UpdatePlayingMarker();
	ModernUI_UpdateTransport();
}

void ModernUI_SetBatch(BOOL bLock)
{
	if (bLock)
	{
		g_iBatchDepth++;
		return;
	}

	if (g_iBatchDepth > 0)
		g_iBatchDepth--;

	if (g_iBatchDepth == 0 && g_bBatchDirty)
	{
		g_bBatchDirty = FALSE;
		RebuildPlaylistRows();
	}
}

////////////////////////////////////////////////////////////////////////////////
// Public API

void ModernUI_Init(HWND hWnd)
{
	INITCOMMONCONTROLSEX icc;
	WNDCLASS wc;
	RECT rc;

	g_hWnd = hWnd;

	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = FooPanelProc;
	wc.hInstance = (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszClassName = FOOCLASS_PANEL;
	RegisterClass(&wc);  // may fail harmlessly if already registered

	GetClientRect(hWnd, &rc);

	g_hPanel = CreateWindowEx(0, FOOCLASS_PANEL, "",
							  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
							  0, 0, rc.right, rc.bottom,
							  hWnd, NULL, wc.hInstance, NULL);

	CreateControls(g_hPanel);
	LayoutControls();
	RebuildPlaylistRows();
	ModernUI_UpdateTransport();
}

void ModernUI_Destroy(void)
{
	if (g_hPanel)
	{
		DestroyWindow(g_hPanel);
		g_hPanel = NULL;
	}

	if (g_hFontUI)
	{
		DeleteObject(g_hFontUI);
		g_hFontUI = NULL;
	}

	g_hBtnStop = g_hBtnPrev = g_hBtnPlay = g_hBtnNext = NULL;
	g_hLblElapsed = g_hLblTotal = g_hSeek = g_hVol = g_hList = g_hStatus = NULL;
	g_hWnd = NULL;
}

void ModernUI_Paint(HWND hWnd)
{
	// The panel covers the whole client area; just validate.
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hWnd, &ps);

	FillRect(hdc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
	EndPaint(hWnd, &ps);
}

void ModernUI_OnSize(HWND hWnd)
{
	RECT rc;

	if (!g_hPanel)
		return;

	GetClientRect(hWnd, &rc);
	MoveWindow(g_hPanel, 0, 0, rc.right, rc.bottom, TRUE);
}

////////////////////////////////////////////////////////////////////////////////
// Legacy compatibility stubs - native controls own all mouse interaction now

void ModernUI_OnLButtonDown(HWND hWnd, POINTS pts)
{
	(void)hWnd; (void)pts;
}

void ModernUI_OnMouseMove(HWND hWnd, POINTS pts, WPARAM wParam)
{
	(void)hWnd; (void)pts; (void)wParam;
}

void ModernUI_OnLButtonUp(HWND hWnd, POINTS pts)
{
	(void)hWnd; (void)pts;
}

BOOL ModernUI_OnRButtonDown(HWND hWnd, POINTS pts)
{
	(void)hWnd; (void)pts;
	return FALSE;
}

BOOL ModernUI_OnMouseWheel(WPARAM wParam, LPARAM lParam)
{
	POINT pt;

	pt.x = (short)LOWORD(lParam);
	pt.y = (short)HIWORD(lParam);

	return RouteMouseWheel(pt, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////

void ModernUI_OnStreamInfo(unsigned int iBitRate_Kbs, unsigned int iFreq_Hz, unsigned int iBitsPerSample, BOOL bStereo)
{
	(void)iBitsPerSample;

	g_fmtBitrateKbs = iBitRate_Kbs;
	g_fmtFreqHz = iFreq_Hz;
	g_fmtStereo = bStereo;
	g_fmtValid = TRUE;

	ModernUI_UpdateTransport();
}

////////////////////////////////////////////////////////////////////////////////
