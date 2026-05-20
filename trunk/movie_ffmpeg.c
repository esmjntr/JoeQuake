/*
movie_ffmpeg.c -- capture_mode raw: write .raw + .pcm to disk
                  capture_mode ffmpeg (Win32): pipe RGB + PCM into ffmpeg.exe -> stem.mp4|mkv

Requirements for capture_mode ffmpeg:
- ffmpeg.exe must sit next to the game executable (same folder as joequake-gl.exe etc.).
- FFmpeg build must expose whatever codecs/options you select in capture_ffmpeg_video_args
  and capture_ffmpeg_audio_args (defaults use libx264 + AAC — common in generic builds).
- Windows only; other platforms: Movie_FFmpeg_Encode_Open returns false.

Video: packed RGB24, GL framebuffer row order (bottom row first). ffmpeg gets -vf vflip.
Audio: interleaved stereo s16le at sample_rate Hz (engine shm->speed).

Encode: two named OUTBOUND pipes (audio then rawvideo). Child stdin is NUL — avoids
stdin/rawvideo coupling and tiny anonymous pipe stalls. CLI lists -i audio pipe before
-i video pipe; ConnectNamedPipe order matches. Failed writes abort capture once.
No +faststart (avoids post-mux rewrite stalls on pipe shutdown).

CreateProcess uses STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST to whitelist
exactly the stdio handles (NUL stdin, stderr log) for inheritance. The pipe SERVER
handles are created with bInheritHandle=TRUE for the same SECURITY_ATTRIBUTES used
by stdio, but the whitelist excludes them so ffmpeg.exe receives only its client
side via CreateFile on the pipe path. Otherwise the inherited unused server handles
in the child keep the pipe alive after the parent CloseHandle, the client read
never sees EOF, and the muxer never finalizes the MP4.

Finalize wait: Movie_FFmpeg_WaitOrKillProcess polls in 100 ms slices and pumps
Sys_SendKeyEvents + SCR_UpdateScreen each iteration so the window stays painted
and Windows does not declare the game unresponsive while ffmpeg writes the moov.
m_finalizing is exposed via Movie_FFmpeg_IsFinalizing so movie.c can refuse new
captures during the wait.
*/

#include "quakedef.h"
#include "movie_ffmpeg.h"

#ifdef _WIN32

#include <stdio.h>
#include <windows.h>

extern void Movie_Stop (void);
extern void Movie_MaybeAutoQuit (void);
extern void Movie_CancelCaptureStats (void);

extern cvar_t capture_ffmpeg_video_buf_mb;
extern cvar_t capture_ffmpeg_audio_buf_mb;
extern cvar_t capture_ffmpeg_loglevel;
extern cvar_t capture_ffmpeg_report;
extern cvar_t capture_ffmpeg_write_timeout_ms;
extern cvar_t capture_ffmpeg_container;
extern cvar_t capture_ffmpeg_video_args;
extern cvar_t capture_ffmpeg_audio_args;

typedef enum {
	FFMPEG_SINK_NONE,
	FFMPEG_SINK_FILES,
	FFMPEG_SINK_ENCODE
} ffmpeg_sink_mode_t;

/* wait for ffmpeg to finish mux after closing pipes; then force-kill (normal: seconds) */
#define FFMPEG_EXIT_WAIT_MS	30000
/* poll granularity for the finalize wait; small enough that the window keeps repainting */
#define FFMPEG_EXIT_POLL_MS	100
/* Fallback args if cvar is empty after trim — keep aligned with defaults in movie.c */
#define CAP_FFMPEG_DEFAULT_VIDEO_ENCODE	"-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p"
#define CAP_FFMPEG_DEFAULT_AUDIO_ENCODE	"-c:a aac -b:a 256k -ar 48000"
#define CAP_FFMPEG_ENCODE_ARG_CAP	6144

static ffmpeg_sink_mode_t	m_sink_mode = FFMPEG_SINK_NONE;
static FILE			*m_ffmpeg_video;
static FILE			*m_ffmpeg_audio;

static HANDLE			m_hVideoPipe		= INVALID_HANDLE_VALUE;
static HANDLE			m_hAudioPipe		= INVALID_HANDLE_VALUE;
static HANDLE			m_hFfmpegProc		= NULL;
static HANDLE			m_hStderrLog		= INVALID_HANDLE_VALUE;
static char			m_stderr_path[MAX_OSPATH];
static char			m_outpath[MAX_OSPATH];
static qboolean			m_encode_aborting	= false;
static qboolean			m_finalizing		= false;
static double			m_finalize_seconds	= 0;

static OVERLAPPED		m_video_ovl;
static OVERLAPPED		m_audio_ovl;

qboolean Movie_FFmpeg_IsFinalizing (void)
{
	return m_finalizing;
}

static void Movie_FFmpeg_Encode_ClosePipes (void)
{
	/*
	 * Plain CloseHandle is the correct graceful shutdown for an outbound named pipe:
	 * the kernel delivers any buffered bytes to the client's next ReadFile, then
	 * the read after that returns 0 (EOF). Do NOT call FlushFileBuffers here - it
	 * blocks until the client drains and would re-introduce the wedge fixed by the
	 * inheritance whitelist. Do NOT call DisconnectNamedPipe either - it discards
	 * unread bytes and surfaces as ERROR_PIPE_NOT_CONNECTED on the client side
	 * rather than clean EOF.
	 */
	if (m_video_ovl.hEvent)
	{
		CloseHandle (m_video_ovl.hEvent);
		m_video_ovl.hEvent = NULL;
	}
	if (m_audio_ovl.hEvent)
	{
		CloseHandle (m_audio_ovl.hEvent);
		m_audio_ovl.hEvent = NULL;
	}
	if (m_hVideoPipe != INVALID_HANDLE_VALUE)
	{
		CloseHandle (m_hVideoPipe);
		m_hVideoPipe = INVALID_HANDLE_VALUE;
	}
	if (m_hAudioPipe != INVALID_HANDLE_VALUE)
	{
		CloseHandle (m_hAudioPipe);
		m_hAudioPipe = INVALID_HANDLE_VALUE;
	}
}

/*
 * Copy trimmed cvar text into dst; blank after trim selects fallback_nonempty.
 */
static qboolean Movie_FFmpeg_CopyTrimmedEncodeArg (const char *raw,
						 const char *fallback_nonempty,
						 char *dst, size_t dstsize)
{
	const char *s = raw ? raw : "";
	const char *e;
	size_t		len;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;

	if (!*s)
	{
		/* Avoid Q_strncpyz here: MSVC C4090 if src were const-qualified in the API. */
		strncpy (dst, fallback_nonempty, dstsize - 1);
		dst[dstsize - 1] = 0;
		return true;
	}

	e = s + strlen (s);
	while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t' || *(e - 1) == '\r'
			 || *(e - 1) == '\n'))
		e--;

	len = (size_t) (e - s);
	if (len >= dstsize)
		return false;

	memcpy (dst, s, len);
	dst[len] = 0;
	return true;
}

static void Movie_FFmpeg_ConsoleHintsAfterEncodeFailure (DWORD wexit)
{
	FILE *f;

	Con_Printf ("ffmpeg exited with code %lu.\n", (unsigned long) wexit);
	if (m_stderr_path[0])
		Con_Printf ("See %s for full FFmpeg stderr.\n", m_stderr_path);
	Con_Printf (
		"If this was an encoder or option error (including \"unknown encoder\"), adjust "
		"capture_ffmpeg_video_args / capture_ffmpeg_audio_args "
		"(defaults use CPU libx264 and AAC).\n");

	if (!m_stderr_path[0])
		return;
	f = fopen (m_stderr_path, "rb");
	if (f)
	{
		char	tail[640];
		size_t	got = 0;
		long	sz;

		memset (tail, 0, sizeof (tail));
		if (fseek (f, 0, SEEK_END) == 0 && (sz = ftell (f)) >= 0)
		{
			long	start = sz > (long)(sizeof (tail) - 1)
					  ? sz - (long)(sizeof (tail) - 1)
					  : 0;
			if (fseek (f, start, SEEK_SET) == 0)
				got = fread (tail, 1, sizeof (tail) - 1, f);
			tail[got] = 0;
			if (Q_strcasestr (tail, "Unknown encoder")
			    || Q_strcasestr (tail, "Codec not found")
			    || Q_strcasestr (tail, "Unrecognized option")
			    || Q_strcasestr (tail, "Error initializing output stream")
			    || Q_strcasestr (tail, "Error while opening encoder")
			    || Q_strcasestr (tail, "Could not open encoder"))
				Con_Printf ("(stderr mentions encoder/options — verify those capture_ffmpeg_*_args strings.)\n");
		}
		fclose (f);
	}
}

static qboolean Movie_FFmpeg_OverlappedWrite (HANDLE pipe, OVERLAPPED *ovl,
					      const void *buf, DWORD nbytes,
					      DWORD timeout_ms)
{
	DWORD	written = 0;
	BOOL	ok;

	if (pipe == INVALID_HANDLE_VALUE || !ovl || !ovl->hEvent)
		return false;

	ResetEvent (ovl->hEvent);
	ok = WriteFile (pipe, buf, nbytes, &written, ovl);
	if (ok)
		return (written == nbytes);
	if (GetLastError () != ERROR_IO_PENDING)
		return false;

	if (WaitForSingleObject (ovl->hEvent, timeout_ms) != WAIT_OBJECT_0)
	{
		CancelIoEx (pipe, ovl);
		WaitForSingleObject (ovl->hEvent, 1000);
		return false;
	}
	if (!GetOverlappedResult (pipe, ovl, &written, FALSE))
		return false;
	return (written == nbytes);
}

static void Movie_FFmpeg_WaitOrKillProcess (const char *timeout_note)
{
	DWORD		wexit;
	double		deadline;
	double		wait_t0;
	qboolean	clean_exit = false;
	qboolean	show_progress;

	if (!m_hFfmpegProc)
	{
		m_finalize_seconds = 0;
		return;
	}

	wait_t0 = Sys_DoubleTime ();
	m_finalize_seconds = 0;

	/* Quiet wait when we just TerminateProcess'd from the abort path: ffmpeg is
	   already dying, no point telling the user we're "finalizing". */
	show_progress = !m_encode_aborting;

	deadline = Sys_DoubleTime () + (FFMPEG_EXIT_WAIT_MS / 1000.0);
	m_finalizing = true;

	if (show_progress)
	{
		Con_Printf ("Finalizing capture, please wait...\n");
		SCR_UpdateScreen ();
	}

	for (;;)
	{
		DWORD waited = WaitForSingleObject (m_hFfmpegProc, FFMPEG_EXIT_POLL_MS);
		if (waited != WAIT_TIMEOUT)
		{
			clean_exit = (waited == WAIT_OBJECT_0);
			break;
		}
		if (Sys_DoubleTime () >= deadline)
			break;
		if (show_progress)
		{
			Sys_SendKeyEvents ();
			SCR_UpdateScreen ();
		}
	}

	if (!clean_exit)
	{
		if (timeout_note)
			Con_Printf ("%s\n", timeout_note);
		TerminateProcess (m_hFfmpegProc, 1);
		WaitForSingleObject (m_hFfmpegProc, 8000);
	}
	else if (GetExitCodeProcess (m_hFfmpegProc, &wexit) && wexit != 0)
	{
		Movie_FFmpeg_ConsoleHintsAfterEncodeFailure (wexit);
	}
	else if (show_progress && m_outpath[0])
	{
		Con_Printf ("capture finalized: %s\n", m_outpath);
	}

	m_finalize_seconds = Sys_DoubleTime () - wait_t0;

	CloseHandle (m_hFfmpegProc);
	m_hFfmpegProc = NULL;
	m_finalizing = false;
}

double Movie_FFmpeg_LastFinalizeSeconds (void)
{
	return m_finalize_seconds;
}

static void Movie_FFmpeg_Encode_AbortFromFailedWrite (const char *msg)
{
	if (m_sink_mode != FFMPEG_SINK_ENCODE || m_encode_aborting)
		return;

	m_encode_aborting = true;
	Con_Printf ("ERROR: %s — stopping capture.\n", msg);

	Movie_FFmpeg_Encode_ClosePipes ();

	if (m_hFfmpegProc)
	{
		TerminateProcess (m_hFfmpegProc, 1);
		Movie_FFmpeg_WaitOrKillProcess (NULL);
	}

	if (m_hStderrLog != INVALID_HANDLE_VALUE)
	{
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
	}

	m_sink_mode = FFMPEG_SINK_NONE;
	m_stderr_path[0] = '\0';
	m_outpath[0] = '\0';

	Movie_CancelCaptureStats ();
	Movie_Stop ();
	Movie_MaybeAutoQuit ();
	m_encode_aborting = false;
}

static void Movie_FFmpeg_GetExeDir (char *out, size_t outsize)
{
	char	mod[MAX_PATH];
	char	*slash;

	if (!GetModuleFileNameA (NULL, mod, sizeof (mod)) || !mod[0])
	{
		out[0] = '\0';
		return;
	}
	Q_strncpyz (out, mod, outsize);
	slash = strrchr (out, '\\');
	if (!slash)
		slash = strrchr (out, '/');
	if (slash)
		slash[1] = '\0';
}

qboolean Movie_FFmpeg_Encode_Open (const char *dir, const char *stem, int width, int height, int fps, int sample_rate)
{
	SECURITY_ATTRIBUTES		sa;
	STARTUPINFOEXA			siex;
	PROCESS_INFORMATION		pi;
	LPPROC_THREAD_ATTRIBUTE_LIST	attrlist = NULL;
	SIZE_T				attrlist_size = 0;
	HANDLE				inherit_handles[2];
	char				exedir[MAX_OSPATH];
	char				ffmpeg_exe[MAX_OSPATH];
	char				outpath[MAX_OSPATH];
	char				pipename_audio[128];
	char				pipename_video[128];
	char				cmdline[16384];
	char				venc_args[CAP_FFMPEG_ENCODE_ARG_CAP];
	char				aenc_args[CAP_FFMPEG_ENCODE_ARG_CAP];
	HANDLE				hNulIn = INVALID_HANDLE_VALUE;
	unsigned long			pid, tick;

	memset (&m_stderr_path, 0, sizeof (m_stderr_path));
	memset (&m_outpath, 0, sizeof (m_outpath));
	Movie_FFmpeg_Close ();

	if (!stem || !stem[0] || width <= 0 || height <= 0 || fps <= 0 || sample_rate <= 0)
		return false;

	Movie_FFmpeg_GetExeDir (exedir, sizeof (exedir));
	if (!exedir[0])
	{
		Con_Printf ("ERROR: Movie_FFmpeg_Encode_Open: GetModuleFileName failed\n");
		return false;
	}
	Q_snprintfz (ffmpeg_exe, sizeof (ffmpeg_exe), "%sffmpeg.exe", exedir);

	if (GetFileAttributesA (ffmpeg_exe) == INVALID_FILE_ATTRIBUTES)
	{
		Con_Printf ("ERROR: %s not found (place ffmpeg.exe next to the game executable)\n", ffmpeg_exe);
		return false;
	}

	sa.nLength = sizeof (sa);
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	{
		const char *ext = capture_ffmpeg_container.string ? capture_ffmpeg_container.string : "mp4";

		if (strcmp (ext, "mp4") != 0 && strcmp (ext, "mkv") != 0)
		{
			Con_Printf ("WARNING: capture_ffmpeg_container '%s' not in {mp4,mkv}, using mp4\n", ext);
			ext = "mp4";
		}
		Q_snprintfz (outpath, sizeof (outpath), "%s/%s.%s", dir, stem, ext);
	}
	COM_CreatePath (outpath);

	Q_snprintfz (m_stderr_path, sizeof (m_stderr_path), "%s/%s_ffmpeg_stderr.txt", dir, stem);
	m_hStderrLog = CreateFileA (
		m_stderr_path,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		&sa,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (m_hStderrLog == INVALID_HANDLE_VALUE)
	{
		Con_Printf ("ERROR: Couldn't create %s\n", m_stderr_path);
		return false;
	}

	pid = (unsigned long) GetCurrentProcessId ();
	tick = (unsigned long) GetTickCount ();
	Q_snprintfz (
		pipename_audio,
		sizeof (pipename_audio),
		"\\\\.\\pipe\\JoeQuakeCap_%lu_%lu_a",
		pid,
		tick);
	Q_snprintfz (
		pipename_video,
		sizeof (pipename_video),
		"\\\\.\\pipe\\JoeQuakeCap_%lu_%lu_v",
		pid,
		tick);

	{
		DWORD abuf = (DWORD) bound (1, capture_ffmpeg_audio_buf_mb.value, 64) * 1024 * 1024;

		/* Halving fallback if the requested size exceeds the per-pipe quota on a
		   constrained machine. Floor at 1 MB so a working pipe is always created. */
		m_hAudioPipe = CreateNamedPipeA (
			pipename_audio,
			PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			abuf,
			abuf,
			0,
			&sa);
		while (m_hAudioPipe == INVALID_HANDLE_VALUE && abuf > 1 * 1024 * 1024)
		{
			abuf /= 2;
			m_hAudioPipe = CreateNamedPipeA (
				pipename_audio,
				PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				1,
				abuf,
				abuf,
				0,
				&sa);
		}
		if (m_hAudioPipe == INVALID_HANDLE_VALUE)
		{
			Con_Printf ("ERROR: CreateNamedPipe (audio) failed (%lu)\n", (unsigned long) GetLastError ());
			CloseHandle (m_hStderrLog);
			m_hStderrLog = INVALID_HANDLE_VALUE;
			return false;
		}
	}

	{
		DWORD vbuf = (DWORD) bound (1, capture_ffmpeg_video_buf_mb.value, 256) * 1024 * 1024;

		m_hVideoPipe = CreateNamedPipeA (
			pipename_video,
			PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1,
			vbuf,
			vbuf,
			0,
			&sa);
		while (m_hVideoPipe == INVALID_HANDLE_VALUE && vbuf > 1 * 1024 * 1024)
		{
			vbuf /= 2;
			m_hVideoPipe = CreateNamedPipeA (
				pipename_video,
				PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				1,
				vbuf,
				vbuf,
				0,
				&sa);
		}
		if (m_hVideoPipe == INVALID_HANDLE_VALUE)
		{
			Con_Printf ("ERROR: CreateNamedPipe (video) failed (%lu)\n", (unsigned long) GetLastError ());
			CloseHandle (m_hAudioPipe);
			m_hAudioPipe = INVALID_HANDLE_VALUE;
			CloseHandle (m_hStderrLog);
			m_hStderrLog = INVALID_HANDLE_VALUE;
			return false;
		}
	}

	memset (&m_video_ovl, 0, sizeof (m_video_ovl));
	memset (&m_audio_ovl, 0, sizeof (m_audio_ovl));
	m_audio_ovl.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);
	m_video_ovl.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);
	if (!m_audio_ovl.hEvent || !m_video_ovl.hEvent)
	{
		Con_Printf ("ERROR: CreateEvent for overlapped pipe I/O failed (%lu)\n", (unsigned long) GetLastError ());
		if (m_audio_ovl.hEvent)
		{
			CloseHandle (m_audio_ovl.hEvent);
			m_audio_ovl.hEvent = NULL;
		}
		if (m_video_ovl.hEvent)
		{
			CloseHandle (m_video_ovl.hEvent);
			m_video_ovl.hEvent = NULL;
		}
		CloseHandle (m_hVideoPipe);
		m_hVideoPipe = INVALID_HANDLE_VALUE;
		CloseHandle (m_hAudioPipe);
		m_hAudioPipe = INVALID_HANDLE_VALUE;
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	hNulIn = CreateFileA (
		"NUL",
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		&sa,
		OPEN_EXISTING,
		0,
		NULL);
	if (hNulIn == INVALID_HANDLE_VALUE)
	{
		Con_Printf ("ERROR: could not open NUL for ffmpeg stdin\n");
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	{
		const char *loglevel = (capture_ffmpeg_loglevel.string && capture_ffmpeg_loglevel.string[0])
				       ? capture_ffmpeg_loglevel.string : "error";
		const char *report   = capture_ffmpeg_report.value ? "-report" : "";

		if (!Movie_FFmpeg_CopyTrimmedEncodeArg (capture_ffmpeg_video_args.string,
						     CAP_FFMPEG_DEFAULT_VIDEO_ENCODE,
						     venc_args, sizeof (venc_args))
		    || !Movie_FFmpeg_CopyTrimmedEncodeArg (capture_ffmpeg_audio_args.string,
							CAP_FFMPEG_DEFAULT_AUDIO_ENCODE,
							aenc_args, sizeof (aenc_args)))
		{
			Con_Printf (
				"ERROR: capture_ffmpeg_video_args or capture_ffmpeg_audio_args "
				"exceed %u characters (trimmed)\n",
				(unsigned) (CAP_FFMPEG_ENCODE_ARG_CAP - 1));
			CloseHandle (hNulIn);
			Movie_FFmpeg_Encode_ClosePipes ();
			CloseHandle (m_hStderrLog);
			m_hStderrLog = INVALID_HANDLE_VALUE;
			return false;
		}

		Q_snprintfz (
			cmdline,
			sizeof (cmdline),
			"\"%s\" -hide_banner -loglevel %s %s -y "
			"-f s16le -ac 2 -ar %d -thread_queue_size 1024 -i %s "
			"-f rawvideo -pixel_format rgb24 -video_size %dx%d -framerate %d "
			"-thread_queue_size 1024 -i %s "
			"-map 0:a -map 1:v -vf vflip -shortest %s %s \"%s\"",
			ffmpeg_exe,
			loglevel,
			report,
			sample_rate,
			pipename_audio,
			width,
			height,
			fps,
			pipename_video,
			venc_args,
			aenc_args,
			outpath);
	}

	memset (&siex, 0, sizeof (siex));
	siex.StartupInfo.cb = sizeof (siex);
	siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	siex.StartupInfo.wShowWindow = SW_HIDE;
	siex.StartupInfo.hStdInput = hNulIn;
	siex.StartupInfo.hStdOutput = m_hStderrLog;
	siex.StartupInfo.hStdError = m_hStderrLog;

	/* Whitelist exactly the stdio handles for inheritance to ffmpeg.exe.
	   Without this the named-pipe SERVER handles are also inherited (they were
	   created with bInheritHandle=TRUE for stdio convenience), which keeps the
	   pipe alive past parent CloseHandle so ffmpeg's client read never sees
	   EOF and the muxer never finalizes. */
	InitializeProcThreadAttributeList (NULL, 1, 0, &attrlist_size);
	attrlist = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc (GetProcessHeap (), 0, attrlist_size);
	if (!attrlist)
	{
		Con_Printf ("ERROR: HeapAlloc for attribute list failed\n");
		CloseHandle (hNulIn);
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	if (!InitializeProcThreadAttributeList (attrlist, 1, 0, &attrlist_size))
	{
		Con_Printf ("ERROR: InitializeProcThreadAttributeList failed (%lu)\n", (unsigned long) GetLastError ());
		HeapFree (GetProcessHeap (), 0, attrlist);
		CloseHandle (hNulIn);
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	inherit_handles[0] = hNulIn;
	inherit_handles[1] = m_hStderrLog;
	if (!UpdateProcThreadAttribute (
		    attrlist,
		    0,
		    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		    inherit_handles,
		    sizeof (inherit_handles),
		    NULL,
		    NULL))
	{
		Con_Printf ("ERROR: UpdateProcThreadAttribute failed (%lu)\n", (unsigned long) GetLastError ());
		DeleteProcThreadAttributeList (attrlist);
		HeapFree (GetProcessHeap (), 0, attrlist);
		CloseHandle (hNulIn);
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	siex.lpAttributeList = attrlist;

	memset (&pi, 0, sizeof (pi));

	if (!CreateProcessA (
		    ffmpeg_exe,
		    cmdline,
		    NULL,
		    NULL,
		    TRUE,
		    EXTENDED_STARTUPINFO_PRESENT,
		    NULL,
		    com_basedir,
		    &siex.StartupInfo,
		    &pi))
	{
		Con_Printf ("ERROR: CreateProcess ffmpeg failed (%lu)\n", (unsigned long) GetLastError ());
		DeleteProcThreadAttributeList (attrlist);
		HeapFree (GetProcessHeap (), 0, attrlist);
		CloseHandle (hNulIn);
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	DeleteProcThreadAttributeList (attrlist);
	HeapFree (GetProcessHeap (), 0, attrlist);
	attrlist = NULL;

	CloseHandle (hNulIn);
	CloseHandle (pi.hThread);
	pi.hThread = NULL;
	m_hFfmpegProc = pi.hProcess;

	if (!ConnectNamedPipe (m_hAudioPipe, NULL) && GetLastError () != ERROR_PIPE_CONNECTED)
	{
		Con_Printf ("ERROR: ConnectNamedPipe (audio) failed (%lu)\n", (unsigned long) GetLastError ());
		TerminateProcess (m_hFfmpegProc, 1);
		WaitForSingleObject (m_hFfmpegProc, 5000);
		CloseHandle (m_hFfmpegProc);
		m_hFfmpegProc = NULL;
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	if (!ConnectNamedPipe (m_hVideoPipe, NULL) && GetLastError () != ERROR_PIPE_CONNECTED)
	{
		Con_Printf ("ERROR: ConnectNamedPipe (video) failed (%lu)\n", (unsigned long) GetLastError ());
		TerminateProcess (m_hFfmpegProc, 1);
		WaitForSingleObject (m_hFfmpegProc, 5000);
		CloseHandle (m_hFfmpegProc);
		m_hFfmpegProc = NULL;
		Movie_FFmpeg_Encode_ClosePipes ();
		CloseHandle (m_hStderrLog);
		m_hStderrLog = INVALID_HANDLE_VALUE;
		return false;
	}

	m_sink_mode = FFMPEG_SINK_ENCODE;
	Q_strncpyz (m_outpath, outpath, sizeof (m_outpath));
	Con_Printf (
		"capture_mode ffmpeg: PCM -ar %i Hz (must match engine rate); video %ix%i @ %i fps; stderr %s\n",
		sample_rate, width, height, fps, m_stderr_path);
	Con_Printf ("capture_mode ffmpeg: %s\n", cmdline);
	Con_Printf ("capture_mode ffmpeg: output %s\n", outpath);
	return true;
}

qboolean Movie_FFmpeg_Open (const char *dir, const char *stem)
{
	char path[MAX_OSPATH];

	Movie_FFmpeg_Close ();

	if (!stem || !stem[0])
		return false;

	m_sink_mode = FFMPEG_SINK_FILES;

	Q_snprintfz (path, sizeof(path), "%s/%s_ffmpeg_video.raw", dir, stem);
	COM_CreatePath (path);
	if (!(m_ffmpeg_video = fopen (path, "wb")))
	{
		Con_Printf ("ERROR: Couldn't open %s\n", path);
		m_sink_mode = FFMPEG_SINK_NONE;
		return false;
	}

	Q_snprintfz (path, sizeof(path), "%s/%s_ffmpeg_audio.pcm", dir, stem);
	COM_CreatePath (path);
	if (!(m_ffmpeg_audio = fopen (path, "wb")))
	{
		Con_Printf ("ERROR: Couldn't open %s\n", path);
		fclose (m_ffmpeg_video);
		m_ffmpeg_video = NULL;
		m_sink_mode = FFMPEG_SINK_NONE;
		return false;
	}

	Con_Printf ("capture_mode raw: writing %s/%s_ffmpeg_*.raw|pcm\n", dir, stem);
	return true;
}

void Movie_FFmpeg_WriteVideo (const byte *pixel_buffer, int size)
{
	if (!pixel_buffer || size <= 0)
		return;

	if (m_sink_mode == FFMPEG_SINK_ENCODE)
	{
		DWORD timeout_ms = (DWORD) bound (100, capture_ffmpeg_write_timeout_ms.value, 60000);

		if (m_hVideoPipe == INVALID_HANDLE_VALUE)
			return;
		if (!Movie_FFmpeg_OverlappedWrite (m_hVideoPipe, &m_video_ovl, pixel_buffer, (DWORD) size, timeout_ms))
			Movie_FFmpeg_Encode_AbortFromFailedWrite ("ffmpeg video pipe write failed or timed out");
		return;
	}

	if (m_sink_mode != FFMPEG_SINK_FILES || !m_ffmpeg_video)
		return;
	if (fwrite (pixel_buffer, 1, size, m_ffmpeg_video) != (size_t) size)
		Con_Printf ("ERROR: ffmpeg video write failed\n");
	fflush (m_ffmpeg_video);
}

void Movie_FFmpeg_WriteAudio (int samples, const byte *sample_buffer)
{
	int	nbytes;

	if (!sample_buffer || samples <= 0)
		return;
	nbytes = samples * 4;

	if (m_sink_mode == FFMPEG_SINK_ENCODE)
	{
		DWORD timeout_ms = (DWORD) bound (100, capture_ffmpeg_write_timeout_ms.value, 60000);

		if (m_hAudioPipe == INVALID_HANDLE_VALUE)
			return;
		if (!Movie_FFmpeg_OverlappedWrite (m_hAudioPipe, &m_audio_ovl, sample_buffer, (DWORD) nbytes, timeout_ms))
			Movie_FFmpeg_Encode_AbortFromFailedWrite ("ffmpeg audio pipe write failed or timed out");
		return;
	}

	if (m_sink_mode != FFMPEG_SINK_FILES || !m_ffmpeg_audio)
		return;
	if (fwrite (sample_buffer, 1, nbytes, m_ffmpeg_audio) != (size_t) nbytes)
		Con_Printf ("ERROR: ffmpeg audio write failed\n");
	fflush (m_ffmpeg_audio);
}

void Movie_FFmpeg_Close (void)
{
	if (m_sink_mode == FFMPEG_SINK_ENCODE)
	{
		Movie_FFmpeg_Encode_ClosePipes ();
		Movie_FFmpeg_WaitOrKillProcess ("ffmpeg did not exit after closing pipes; forcing termination");

		if (m_hStderrLog != INVALID_HANDLE_VALUE)
		{
			CloseHandle (m_hStderrLog);
			m_hStderrLog = INVALID_HANDLE_VALUE;
		}
		m_sink_mode = FFMPEG_SINK_NONE;
		m_stderr_path[0] = '\0';
		m_outpath[0] = '\0';
		return;
	}

	if (m_ffmpeg_video)
	{
		fclose (m_ffmpeg_video);
		m_ffmpeg_video = NULL;
	}
	if (m_ffmpeg_audio)
	{
		fclose (m_ffmpeg_audio);
		m_ffmpeg_audio = NULL;
	}
	m_sink_mode = FFMPEG_SINK_NONE;
}

#else /* !_WIN32 */

qboolean Movie_FFmpeg_Encode_Open (const char *dir, const char *stem, int width, int height, int fps, int sample_rate)
{
	(void)dir;
	(void)stem;
	(void)width;
	(void)height;
	(void)fps;
	(void)sample_rate;
	return false;
}

qboolean Movie_FFmpeg_Open (const char *dir, const char *stem)
{
	(void)dir;
	(void)stem;
	return false;
}

void Movie_FFmpeg_WriteVideo (const byte *pixel_buffer, int size)
{
	(void)pixel_buffer;
	(void)size;
}

void Movie_FFmpeg_WriteAudio (int samples, const byte *sample_buffer)
{
	(void)samples;
	(void)sample_buffer;
}

void Movie_FFmpeg_Close (void)
{
}

qboolean Movie_FFmpeg_IsFinalizing (void)
{
	return false;
}

double Movie_FFmpeg_LastFinalizeSeconds (void)
{
	return 0;
}

#endif
