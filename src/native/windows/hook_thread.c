/* JNativeHook: Global keyboard and mouse hooking for Java.
 * Copyright (C) 2006-2013 Alexander Barker.  All Rights Received.
 * http://code.google.com/p/jnativehook/
 *
 * JNativeHook is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * JNativeHook is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <nativehook.h>
#include <windows.h>

#include "convert_to_native.h"
#include "convert_to_virtual.h"
#include "hook_callback.h"
#include "win_unicode_helper.h"

// The handle to the DLL module pulled in DllMain on DLL_PROCESS_ATTACH.
extern HINSTANCE hInst;

// Click count globals.
static unsigned short click_count = 0;
static DWORD click_time = 0;
static POINT last_click;

// Thread and hook handles.
static DWORD hook_thread_id = 0;
static HANDLE hook_thread_handle = NULL, hook_control_handle = NULL;
static HHOOK keyboard_event_hhook = NULL, mouse_event_hhook = NULL;

static DWORD WINAPI hook_thread_proc(LPVOID UNUSED(lpParameter)) {
	DWORD status = NATIVEHOOK_FAILURE;

	// Create the native hooks.
	keyboard_event_hhook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_event_proc, hInst, 0);
	mouse_event_hhook = SetWindowsHookEx(WH_MOUSE_LL, mouse_event_proc, hInst, 0);

	// If we did not encounter a problem, start processing events.
	if (keyboard_event_hhook != NULL && mouse_event_hhook != NULL) {
		#ifdef DEBUG
		fprintf(stdout, "hook_thread_proc(): SetWindowsHookEx() successful.\n");
		#endif

		// Check and setup modifiers.
		if (GetKeyState(VK_LSHIFT)	 < 0)	SetModifierMask(MOD_LSHIFT);
		if (GetKeyState(VK_RSHIFT)   < 0)	SetModifierMask(MOD_RSHIFT);
		if (GetKeyState(VK_LCONTROL) < 0)	SetModifierMask(MOD_LCONTROL);
		if (GetKeyState(VK_RCONTROL) < 0)	SetModifierMask(MOD_RCONTROL);
		if (GetKeyState(VK_LMENU)    < 0)	SetModifierMask(MOD_LALT);
		if (GetKeyState(VK_RMENU)    < 0)	SetModifierMask(MOD_RALT);
		if (GetKeyState(VK_LWIN)     < 0)	SetModifierMask(MOD_LWIN);
		if (GetKeyState(VK_RWIN)     < 0)	SetModifierMask(MOD_RWIN);

		// Set the exit status.
		status = NATIVEHOOK_SUCCESS;

		// Signal that we have passed the thread initialization.
		SetEvent(hook_control_handle);

		// Block until the thread receives an WM_QUIT request.
		MSG message;
		while (GetMessage(&message, (HWND) -1, 0, 0) > 0) {
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
	}
	else {
		#ifdef DEBUG
		// TODO Print the error message.
		fprintf(stderr, "hook_thread_proc(): SetWindowsHookEx() failed!\n");
		#endif

		status = NATIVEHOOK_ERROR_SET_WINDOWS_HOOK_EX;
	}

	// Destroy the native hooks.
	if (keyboard_event_hhook != NULL) {
		UnhookWindowsHookEx(keyboard_event_hhook);
		keyboard_event_hhook = NULL;
	}

	if (mouse_event_hhook != NULL) {
		UnhookWindowsHookEx(mouse_event_hhook);
		mouse_event_hhook = NULL;
	}

	#ifdef DEBUG
	fprintf(stdout, "hook_thread_proc(): complete.\n");
	#endif

	// Make sure we signal that we have passed any exception throwing code.
	// This should only make a difference if we had an initialization exception.
	SetEvent(hook_control_handle);

	ExitThread(status);
}

NATIVEHOOK_API int hook_enable() {
	int status = NATIVEHOOK_FAILURE;

	// Make sure the native thread is not already running.
	if (IsNativeThreadRunning() != true) {
		// Create event handle for the thread hook.
		hook_control_handle = CreateEvent(NULL, TRUE, FALSE, "hook_control_handle");

		LPTHREAD_START_ROUTINE lpStartAddress = &hook_thread_proc;
		hook_thread_handle = CreateThread(NULL, 0, lpStartAddress, NULL, 0, &hook_thread_id);
		if (hook_thread_handle != INVALID_HANDLE_VALUE) {
			#ifdef DEBUG
			fprintf(stdout, "StartNativeThread(): start successful.\n");
			#endif

			// Wait for any possible thread exceptions to get thrown into
			// the queue
			WaitForSingleObject(hook_control_handle, INFINITE);

			// TODO Set the return status to the thread exit code.
			if (IsNativeThreadRunning()) {
				#ifdef DEBUG
				fprintf(stdout, "StartNativeThread(): initialization successful.\n");
				#endif

				status = NATIVEHOOK_SUCCESS;
			}
			else {
				#ifdef DEBUG
				fprintf(stderr, "StartNativeThread(): initialization failure!\n");
				#endif

				// Wait for the thread to die.
				WaitForSingleObject(hook_thread_handle,  INFINITE);

				DWORD thread_status;
				GetExitCodeThread(hook_thread_handle, &thread_status);
				status = (int) thread_status;

				#ifdef DEBUG
				fprintf(stderr, "StartNativeThread(): Thread Result (%i)\n", status);
				#endif

				if (thread_ex.class != NULL && thread_ex.message != NULL)  {
					ThrowException(thread_ex.class, thread_ex.message);
				}
			}
		}
		else {
			#ifdef DEBUG
			fprintf(stderr, "StartNativeThread(): start failure!\n");
			#endif

			ThrowException(NATIVE_HOOK_EXCEPTION, "Native thread start failure");
		}
	}

	return status;
}

NATIVEHOOK_API int hook_disable() {
	int status = NATIVEHOOK_FAILURE;

	if (hook_is_enabled() == true) {
		// Try to exit the thread naturally.
		PostThreadMessage(hook_thread_id, WM_QUIT, (WPARAM) NULL, (LPARAM) NULL);
		WaitForSingleObject(hook_thread_handle,  INFINITE);

		DWORD thread_status;
		GetExitCodeThread(hook_thread_handle, &thread_status);
		status = (int) thread_status;

		CloseHandle(hook_thread_handle);
		hook_thread_handle = NULL;

		CloseHandle(hook_control_handle);
		hook_control_handle = NULL;

		#ifdef DEBUG
		fprintf(stdout, "StopNativeThread(): Thread Result (%i)\n", status);
		#endif
	}

	return status;
}

NATIVEHOOK_API bool hook_is_enabled() {
	bool is_running = false;

	DWORD status;
	GetExitCodeThread(hook_thread_handle, &status);

	if (status == STILL_ACTIVE) {
		is_running = true;
	}

	#ifdef DEBUG
	fprintf(stdout, "IsNativeThreadRunning(): State (%i)\n", is_running);
	#endif

	return is_running;
}
