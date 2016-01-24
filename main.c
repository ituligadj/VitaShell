/*
	VitaShell
	Copyright (C) 2015-2016, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	TODO:
	- Nethost. Patch UVL to be able to launch from host0
	- Terminate thread / free stack of previous VitaShell when reloading
	- Fix gxm bug in homebrew.c
	- Page skip for hex and text viewer
	- Improve homebrew exiting. Compatibility list at http://wololo.net/talk/viewtopic.php?f=113&p=402975#p402975
	- Add UTF8/UTF16 to vita2dlib's pgf
	- Maybe switch to libarchive
	- Hex editor byte group size
	- CPU changement
	- Move error fix by moving its content
	- Duplicate when same location or same folder
	- Copy same location check and handling. x:/a to x:/a/b loop.
	- Shortcuts
	- Media player
	- Maybe auto-exiting copying screen
*/

#include "main.h"
#include "init.h"
#include "homebrew.h"
#include "io_process.h"
#include "archive.h"
#include "photo.h"
#include "file.h"
#include "text.h"
#include "hex.h"
#include "ftp.h"
#include "message_dialog.h"
#include "ime_dialog.h"
#include "language.h"
#include "utils.h"
#include "module.h"
#include "psp/pboot.h"

#ifdef RELEASE
#include "splashscreen.h"
#endif

int _newlib_heap_size_user = 32 * 1024 * 1024;

#define MAX_DIR_LEVELS 1024

// File lists
static FileList file_list, mark_list, copy_list;

// Paths
static char cur_file[MAX_PATH_LENGTH], cur_path[MAX_PATH_LENGTH], copy_path[MAX_PATH_LENGTH], archive_path[MAX_PATH_LENGTH];

// Mount point stat
static SceIoStat mount_point_stat;

// Position
static int base_pos = 0, rel_pos = 0;
static int base_pos_list[MAX_DIR_LEVELS];
static int rel_pos_list[MAX_DIR_LEVELS];
static int dir_level = 0;

// Copy mode
static int copy_mode = COPY_MODE_NORMAL;

// Archive
static int is_in_archive = 0;
static int dir_level_archive = -1;

// Context menu
static int ctx_menu_mode = CONTEXT_MENU_CLOSED;
static int ctx_menu_pos = -1;
static float ctx_menu_width = 0;
static float ctx_menu_max_width = 0;

// Net info
static SceNetEtherAddr mac;
static char ip[16];

// FTP
static char vita_ip[16];
static unsigned short int vita_port;

// Enter and cancel buttons
int SCE_CTRL_ENTER = SCE_CTRL_CROSS, SCE_CTRL_CANCEL = SCE_CTRL_CIRCLE;

// Dialog step
int dialog_step = DIALOG_STEP_NONE;

void dirLevelUp() {
	base_pos_list[dir_level] = base_pos;
	rel_pos_list[dir_level] = rel_pos;
	dir_level++;
	base_pos_list[dir_level] = 0;
	rel_pos_list[dir_level] = 0;
	base_pos = 0;
	rel_pos = 0;
}

int isInArchive() {
	return is_in_archive;
}

void dirUpCloseArchive() {
	if (isInArchive() && dir_level_archive >= dir_level) {
		is_in_archive = 0;
		archiveClose();
		dir_level_archive = -1;
	}
}

void dirUp() {
	removeEndSlash(cur_path);

	char *p;

	p = strrchr(cur_path, '/');
	if (p) {
		p[1] = '\0';
		dir_level--;
		goto DIR_UP_RETURN;
	}

	p = strrchr(cur_path, ':');
	if (p) {
		if (strlen(cur_path) - ((p + 1) - cur_path) > 0) {
			p[1] = '\0';
			dir_level--;
			goto DIR_UP_RETURN;
		}
	}

	strcpy(cur_path, HOME_PATH);
	dir_level = 0;

DIR_UP_RETURN:
	base_pos = base_pos_list[dir_level];
	rel_pos = rel_pos_list[dir_level];
	dirUpCloseArchive();
}

void refreshFileList() {
	int res = 0;

	do {
		fileListEmpty(&file_list);

		res = fileListGetEntries(&file_list, cur_path);

		if (res < 0)
			dirUp();
	} while (res < 0);

	while (!fileListGetNthEntry(&file_list, base_pos + rel_pos)) {
		if (rel_pos > 0) {
			rel_pos--;
		} else {
			if (base_pos > 0) {
				base_pos--;
			}
		}
	}
}

void refreshMarkList() {
	FileListEntry *entry = mark_list.head;

	int length = mark_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		sprintf(path, "%s%s", cur_path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		if (sceIoGetstat(path, &stat) < 0)
			fileListRemoveEntry(&mark_list, entry->name);

		// Next
		entry = next;
	}
}

void refreshCopyList() {
	FileListEntry *entry = copy_list.head;

	int length = copy_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		sprintf(path, "%s%s", copy_path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		if (sceIoGetstat(path, &stat) < 0)
			fileListRemoveEntry(&copy_list, entry->name);

		// Next
		entry = next;
	}
}

void resetFileLists() {
	memset(&file_list, 0, sizeof(FileList));
	memset(&mark_list, 0, sizeof(FileList));
	memset(&copy_list, 0, sizeof(FileList));

	refreshFileList();
}

/*
	TODO:
	- Check if at /PSP/GAME/
	- Check if path is <= 10?
	- Convert PNG to 24bit?
*/
int signPspFile(char *file) {
	SceUID fd = sceIoOpen(file, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	PBPHeader header;
	sceIoRead(fd, &header, sizeof(PBPHeader));

	uint32_t magic;
	sceIoLseek(fd, header.elf_offset, SCE_SEEK_SET);
	sceIoRead(fd, &magic, sizeof(uint32_t));
	sceIoClose(fd);

	if (magic != 0x464C457F) {
		infoDialog(language_container[SIGN_ERROR]);
		return -1;
	}

	char path[MAX_PATH_LENGTH];

	strcpy(path, file);

	char *p = strrchr(path, '/');
	if (p) {
		*p = '\0';

		char *q = strrchr(path, '/');
		if (q) {
			char name[MAX_NAME_LENGTH];
			strcpy(name, q + 1);

			// Set disc id
			setDiscId(name);

			// Write PBOOT.PBP
			strcpy(p, "/PBOOT.PBP");
			int res = writePboot(file, path);
			if (res >= 0) {
				// Rename to EBOOT_ORI.PBP
				strcpy(p, "/EBOOT_ORI.PBP");
				sceIoRename(file, path);
			} else {
				sceIoRemove(path);
				return res;
			}
		}
	}

	return 0;
}

int handleFile(char *file) {
	int res = 0;

	int type = getFileType(file);

	switch (type) {
		case FILE_TYPE_ELF:
		case FILE_TYPE_PBP:
		case FILE_TYPE_RAR:
		case FILE_TYPE_7ZIP:
		case FILE_TYPE_ZIP:
			if (isInArchive())
				type = FILE_TYPE_UNKNOWN;

			break;
	}

	switch (type) {
		case FILE_TYPE_UNKNOWN:
			res = textViewer(file);
			break;

		case FILE_TYPE_ELF:
			if (isValidElf(file)) {
				loadHomebrew(file);
			} else {
				res = textViewer(file);
			}

			break;

		case FILE_TYPE_BMP:
		case FILE_TYPE_PNG:
		case FILE_TYPE_JPEG:
			res = photoViewer(file, type);
			break;

		case FILE_TYPE_PBP:
			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[SIGN_QUESTION]);
			dialog_step = DIALOG_STEP_SIGN_CONFIRM;
			break;

		case FILE_TYPE_RAR:
		case FILE_TYPE_7ZIP:
		case FILE_TYPE_ZIP:
			archiveOpen(file);
			break;

		default:
			errorDialog(type);
			break;
	}

	if (res < 0)
		errorDialog(res);

	return type;
}

void drawScrollBar(int pos, int n) {
	if (n > MAX_POSITION) {
		vita2d_draw_rectangle(SCROLL_BAR_X, START_Y, SCROLL_BAR_WIDTH, MAX_ENTRIES * FONT_Y_SPACE, GRAY);

		float y = START_Y + ((pos * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);
		float height = ((MAX_POSITION * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);

		vita2d_draw_rectangle(SCROLL_BAR_X, MIN(y, (START_Y + MAX_ENTRIES * FONT_Y_SPACE - height)), SCROLL_BAR_WIDTH, MAX(height, SCROLL_BAR_MIN_HEIGHT), AZURE);
	}
}

void drawShellInfo(char *path) {
	// Title
	vita2d_pgf_draw_textf(font, SHELL_MARGIN_X, SHELL_MARGIN_Y, VIOLET, FONT_SIZE, "VitaShell %d.%d", VITASHELL_VERSION_MAJOR, VITASHELL_VERSION_MINOR);

	// Battery
	float battery_x = ALIGN_LEFT(SCREEN_WIDTH - SHELL_MARGIN_X, vita2d_texture_get_width(battery_image));
	vita2d_draw_texture(battery_image, battery_x, SHELL_MARGIN_Y + 3.0f);

	vita2d_texture *battery_bar_image = battery_bar_green_image;

	float percent = scePowerGetBatteryLifePercent() / 100.0f;

	if (percent <= 0.2f)
		battery_bar_image = battery_bar_red_image;

	float width = vita2d_texture_get_width(battery_bar_image);
	vita2d_draw_texture_part(battery_bar_image, battery_x + 3.0f + (1.0f - percent) * width, SHELL_MARGIN_Y + 5.0f, (1.0f - percent) * width, 0.0f, percent * width, vita2d_texture_get_height(battery_bar_image));

	// Date & time
	SceRtcTime time;
	sceRtcGetCurrentClockLocalTime(&time);

	char date_string[16];
	getDateString(date_string, date_format, &time);

	char time_string[24];
	getTimeString(time_string, time_format, &time);

	char string[64];
	sprintf(string, "%s  %s", date_string, time_string);
	vita2d_pgf_draw_text(font, ALIGN_LEFT(battery_x - 12.0f, vita2d_pgf_text_width(font, FONT_SIZE, string)), SHELL_MARGIN_Y, WHITE, FONT_SIZE, string);

	// TODO: make this more elegant
	// Path
	int line_width = 0;

	int i;
	for (i = 0; i < strlen(path); i++) {
		char ch_width = font_size_cache[(int)path[i]];

		// Too long
		if ((line_width + ch_width) >= MAX_WIDTH)
			break;

		// Increase line width
		line_width += ch_width;
	}

	char path_first_line[256], path_second_line[256];

	strncpy(path_first_line, path, i);
	path_first_line[i] = '\0';

	strcpy(path_second_line, path + i);

	vita2d_pgf_draw_text(font, SHELL_MARGIN_X, PATH_Y, LITEGRAY, FONT_SIZE, path_first_line);
	vita2d_pgf_draw_text(font, SHELL_MARGIN_X, PATH_Y + FONT_Y_SPACE, LITEGRAY, FONT_SIZE, path_second_line);
}

enum MenuEntrys {
	MENU_ENTRY_MARK_UNMARK_ALL,
	MENU_ENTRY_EMPTY_1,
	MENU_ENTRY_MOVE,
	MENU_ENTRY_COPY,
	MENU_ENTRY_PASTE,
	MENU_ENTRY_EMPTY_2,
	MENU_ENTRY_DELETE,
	MENU_ENTRY_RENAME,
	MENU_ENTRY_EMPTY_3,
	MENU_ENTRY_NEW_FOLDER,
	MENU_ENTRY_EMPTY_4,
	//MENU_ENTRY_SEND_BY_EMAIL,
};

enum MenuVisibilities {
	VISIBILITY_UNUSED,
	VISIBILITY_INVISIBLE,
	VISIBILITY_VISIBLE,
};

typedef struct {
	int name;
	int visibility;
} MenuEntry;

MenuEntry menu_entries[] = {
	{ MARK_ALL, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
	{ MOVE, VISIBILITY_INVISIBLE },
	{ COPY, VISIBILITY_INVISIBLE },
	{ PASTE, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
	{ DELETE, VISIBILITY_INVISIBLE },
	{ RENAME, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
	{ NEW_FOLDER, VISIBILITY_INVISIBLE },
//	{ -1, VISIBILITY_UNUSED },
//	{ "Send by Email", VISIBILITY_INVISIBLE },
};

#define N_MENU_ENTRIES (sizeof(menu_entries) / sizeof(MenuEntry))

void initContextMenu() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility == VISIBILITY_INVISIBLE)
			menu_entries[i].visibility = VISIBILITY_VISIBLE;
	}

	FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

	// Invisble entries when on '..'
	if (strcmp(file_entry->name, DIR_UP) == 0) {
		menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_MOVE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_COPY].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_DELETE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_RENAME].visibility = VISIBILITY_INVISIBLE;
		//menu_entries[MENU_ENTRY_SEND_BY_EMAIL].visibility = VISIBILITY_INVISIBLE;
	}

	// Invisible 'Send by Email' when on directory or on file bigger than 2MB
	//if (file_entry->is_folder || file_entry->size >= 2 * 1024 * 1024)
	//	menu_entries[MENU_ENTRY_SEND_BY_EMAIL].visibility = VISIBILITY_INVISIBLE;

	// Invisible 'Paste' if nothing is copied yet
	if (copy_list.length == 0)
		menu_entries[MENU_ENTRY_PASTE].visibility = VISIBILITY_INVISIBLE;

	// Invisble write operations in archives or read-only mount points
	if (isInArchive() || !(mount_point_stat.st_mode & SCE_S_IWUSR)) {
		menu_entries[MENU_ENTRY_MOVE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_PASTE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_DELETE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_RENAME].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_NEW_FOLDER].visibility = VISIBILITY_INVISIBLE;
	}

	// TODO: Moving from one mount point to another is not possible

	// Mark/Unmark all text
	if (mark_list.length == (file_list.length - 1)) { // All marked
		menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
	} else { // Not all marked yet
		// On marked entry
		if (fileListFindEntry(&mark_list, file_entry->name)) {
			menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
		} else {
			menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = MARK_ALL;
		}
	}

	// Go to first entry
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
			ctx_menu_pos = i;
			break;
		}
	}

	if (i == N_MENU_ENTRIES)
		ctx_menu_pos = -1;
}

float easeOut(float x0, float x1, float a) {
	float dx = (x1 - x0);
	return ((dx * a) > 0.5f) ? (dx * a) : dx;
}

void drawContextMenu() {
	// Easing out
	if (ctx_menu_mode == CONTEXT_MENU_CLOSING) {
		if (ctx_menu_width > 0.0f) {
			ctx_menu_width -= easeOut(0.0f, ctx_menu_width, 0.375f);
		} else {
			ctx_menu_mode = CONTEXT_MENU_CLOSED;
		}
	}

	if (ctx_menu_mode == CONTEXT_MENU_OPENING) {
		if (ctx_menu_width < ctx_menu_max_width) {
			ctx_menu_width += easeOut(ctx_menu_width, ctx_menu_max_width, 0.375f);
		} else {
			ctx_menu_mode = CONTEXT_MENU_OPENED;
		}
	}

	// Draw context menu
	if (ctx_menu_mode != CONTEXT_MENU_CLOSED) {
		vita2d_draw_rectangle(SCREEN_WIDTH - ctx_menu_width, 0.0f, ctx_menu_width, SCREEN_HEIGHT, COLOR_ALPHA(0xFF2F2F2F, 0xFA));

		int i;
		for (i = 0; i < N_MENU_ENTRIES; i++) {
			if (menu_entries[i].visibility == VISIBILITY_UNUSED)
				continue;

			float y = START_Y + (i * FONT_Y_SPACE);

			uint32_t color = WHITE;

			if (i == ctx_menu_pos)
				color = GREEN;

			if (menu_entries[i].visibility == VISIBILITY_INVISIBLE)
				color = DARKGRAY;

			vita2d_pgf_draw_text(font, SCREEN_WIDTH - ctx_menu_width + CONTEXT_MENU_MARGIN, y, color, FONT_SIZE, language_container[menu_entries[i].name]);
		}
	}
}

void contextMenuCtrl() {
	if (hold_buttons & SCE_CTRL_UP || hold2_buttons & SCE_CTRL_LEFT_ANALOG_UP) {
		int i;
		for (i = N_MENU_ENTRIES - 1; i >= 0; i--) {
			if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
				if (i < ctx_menu_pos) {
					ctx_menu_pos = i;
					break;
				}
			}
		}
	} else if (hold_buttons & SCE_CTRL_DOWN || hold2_buttons & SCE_CTRL_LEFT_ANALOG_DOWN) {
		int i;
		for (i = 0; i < N_MENU_ENTRIES; i++) {
			if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
				if (i > ctx_menu_pos) {
					ctx_menu_pos = i;
					break;
				}
			}
		}
	}

	// Back
	if (pressed_buttons & SCE_CTRL_TRIANGLE || pressed_buttons & SCE_CTRL_CANCEL) {
		ctx_menu_mode = CONTEXT_MENU_CLOSING;
	}

	// Handle
	if (pressed_buttons & SCE_CTRL_ENTER) {
		switch (ctx_menu_pos) {
			case MENU_ENTRY_MARK_UNMARK_ALL:
			{
				int on_marked_entry = 0;
				int length = mark_list.length;

				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
				if (fileListFindEntry(&mark_list, file_entry->name))
					on_marked_entry = 1;

				// Empty mark list
				fileListEmpty(&mark_list);

				// Mark all if not all entries are marked yet and we are not focusing on a marked entry
				if (length != (file_list.length - 1) && !on_marked_entry) {
					FileListEntry *file_entry = file_list.head->next; // Ignore '..'

					int i;
					for (i = 0; i < file_list.length - 1; i++) {
						FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
						memcpy(mark_entry, file_entry, sizeof(FileListEntry));
						fileListAddEntry(&mark_list, mark_entry, SORT_NONE);

						// Next
						file_entry = file_entry->next;
					}
				}

				break;
			}

			case MENU_ENTRY_MOVE:
			case MENU_ENTRY_COPY:
			{
				// Mode
				if (ctx_menu_pos == MENU_ENTRY_MOVE) {
					copy_mode = COPY_MODE_MOVE;
				} else {
					copy_mode = isInArchive() ? COPY_MODE_EXTRACT : COPY_MODE_NORMAL;
				}

				// Empty copy list at first
				if (copy_list.length > 0)
					fileListEmpty(&copy_list);

				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// Paths
				if (fileListFindEntry(&mark_list, file_entry->name)) { // On marked entry
					// Copy mark list to copy list
					FileListEntry *mark_entry = mark_list.head;

					int i;
					for (i = 0; i < mark_list.length; i++) {
						FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
						memcpy(copy_entry, mark_entry, sizeof(FileListEntry));
						fileListAddEntry(&copy_list, copy_entry, SORT_NONE);

						// Next
						mark_entry = mark_entry->next;
					}
				} else {
					FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
					memcpy(copy_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&copy_list, copy_entry, SORT_NONE);
				}

				strcpy(copy_path, cur_path);

				char string[128];
				sprintf(string, language_container[COPY_MESSAGE], copy_list.length);
				infoDialog(string);

				break;
			}

			case MENU_ENTRY_PASTE:
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[copy_mode == COPY_MODE_MOVE ? MOVING : COPYING]);
				dialog_step = DIALOG_STEP_PASTE;
				break;

			case MENU_ENTRY_DELETE:
			{
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[file_entry->is_folder ? DELETE_FOLDER_QUESTION : DELETE_FILE_QUESTION]);
				dialog_step = DIALOG_STEP_DELETE_CONFIRM;
				break;
			}

			case MENU_ENTRY_RENAME:
			{
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				char name[MAX_NAME_LENGTH];
				strcpy(name, file_entry->name);
				removeEndSlash(name);

				initImeDialog(language_container[RENAME], name, MAX_NAME_LENGTH);

				dialog_step = DIALOG_STEP_RENAME;
				break;
			}

			case MENU_ENTRY_NEW_FOLDER:
				initImeDialog(language_container[NEW_FOLDER], language_container[NEW_FOLDER], MAX_NAME_LENGTH);
				dialog_step = DIALOG_STEP_NEW_FOLDER;
				break;
				/*
			case MENU_ENTRY_SEND_BY_EMAIL:
			{
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				char uri[MAX_PATH_LENGTH];
				sprintf(uri, "email:send?attach=%s%s.", cur_path, file_entry->name);
				debugPrintf("%s\n", uri);
				sceAppMgrLaunchAppByUri(0xFFFFF, uri);
				break;
			}*/
		}

		ctx_menu_mode = CONTEXT_MENU_CLOSING;
	}
}

int dialogSteps() {
	int refresh = 0;

	int msg_result = updateMessageDialog();
	int ime_result = updateImeDialog();

	switch (dialog_step) {
		// Without refresh
		case DIALOG_STEP_ERROR:
		case DIALOG_STEP_INFO:
		case DIALOG_STEP_SYSTEM:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		// With refresh
		case DIALOG_STEP_SIGNED:
		case DIALOG_STEP_COPIED:
		case DIALOG_STEP_DELETED:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_MOVED:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				fileListEmpty(&copy_list);
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_FTP:
			disableAutoSuspend();

			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				ftp_fini();
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_SIGN_CONFIRM:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				int res = signPspFile(cur_file);
				if (res >= 0) {
					initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK, language_container[SIGN_SUCCESS]);
					dialog_step = DIALOG_STEP_SIGNED;
				} else {
					errorDialog(res);
				}
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_NEW_FOLDER:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (strlen(name) > 0) {
					char path[MAX_PATH_LENGTH];
					sprintf(path, "%s%s", cur_path, name);

					int res = sceIoMkdir(path, 0777);
					if (res < 0) {
						errorDialog(res);
					} else {
						refresh = 1;
						dialog_step = DIALOG_STEP_NONE;
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_PASTE:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				CopyArguments args;
				args.file_list = &file_list;
				args.copy_list = &copy_list;
				args.cur_path = cur_path;
				args.copy_path = copy_path;
				args.archive_path = archive_path;
				args.copy_mode = copy_mode;

				SceUID thid = sceKernelCreateThread("copy_thread", (SceKernelThreadEntry)copy_thread, 0x10000100, 0x10000, 0, 0x70000, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(CopyArguments), &args);

				dialog_step = DIALOG_STEP_COPYING;
			}

			break;

		case DIALOG_STEP_DELETE_CONFIRM:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[DELETING]);
				dialog_step = DIALOG_STEP_DELETE_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_DELETE_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				DeleteArguments args;
				args.file_list = &file_list;
				args.mark_list = &mark_list;
				args.cur_path = cur_path;
				args.index = base_pos + rel_pos;

				SceUID thid = sceKernelCreateThread("delete_thread", (SceKernelThreadEntry)delete_thread, 0x10000100, 0x10000, 0, 0x70000, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(DeleteArguments), &args);

				dialog_step = DIALOG_STEP_DELETING;
			}

			break;

		case DIALOG_STEP_RENAME:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (strlen(name) > 0) {
					FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

					char old_path[MAX_PATH_LENGTH];
					char new_path[MAX_PATH_LENGTH];

					sprintf(old_path, "%s%s", cur_path, file_entry->name);
					sprintf(new_path, "%s%s", cur_path, name);

					int res = sceIoRename(old_path, new_path);
					if (res < 0) {
						errorDialog(res);
					} else {
						refresh = 1;
						dialog_step = DIALOG_STEP_NONE;
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
	}

	return refresh;
}

void fileBrowserMenuCtrl() {
/*
	if (pressed_buttons & SCE_CTRL_START) {
		SwVersionParam sw_ver_param;
		sw_ver_param.size = sizeof(SwVersionParam);
		sceKernelGetSystemSwVersion(&sw_ver_param);

		char mac_string[32];
		sprintf(mac_string, "%02X:%02X:%02X:%02X:%02X:%02X", mac.data[0], mac.data[1], mac.data[2], mac.data[3], mac.data[4], mac.data[5]);

		uint64_t free_size = 0, max_size = 0;
		sceAppMgrGetDevInfo("ux0:", &max_size, &free_size);

		char free_size_string[16], max_size_string[16];
		getSizeString(free_size_string, free_size);
		getSizeString(max_size_string, max_size);

		initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK, "System software: %s\nMAC address: %s\nIP address: %s\nMemory card: %s/%s", sw_ver_param.version_string, mac_string, ip, free_size_string, max_size_string);
		dialog_step = DIALOG_STEP_SYSTEM;
	}
*/
/*
	if (pressed_buttons & SCE_CTRL_LTRIGGER) {
		listMemBlocks(0x60000000, 0xD0000000);
	}
*/
	if (pressed_buttons & SCE_CTRL_SELECT) {
		if (!ftp_is_initialized()) {
			int res = ftp_init(vita_ip, &vita_port);
			if (res < 0) {
				infoDialog(language_container[WIFI_ERROR]);
			} else {
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[FTP_SERVER], vita_ip, vita_port);
				dialog_step = DIALOG_STEP_FTP;
			}
		}
	}

	// Move
	if (hold_buttons & SCE_CTRL_UP || hold2_buttons & SCE_CTRL_LEFT_ANALOG_UP) {
		if (rel_pos > 0) {
			rel_pos--;
		} else {
			if (base_pos > 0) {
				base_pos--;
			}
		}
	} else if (hold_buttons & SCE_CTRL_DOWN || hold2_buttons & SCE_CTRL_LEFT_ANALOG_DOWN) {
		if ((rel_pos + 1) < file_list.length) {
			if ((rel_pos + 1) < MAX_POSITION) {
				rel_pos++;
			} else {
				if ((base_pos + rel_pos + 1) < file_list.length) {
					base_pos++;
				}
			}
		}
	}

	if (dir_level > 0) {
		// Context menu trigger
		if (pressed_buttons & SCE_CTRL_TRIANGLE) {
			if (ctx_menu_mode == CONTEXT_MENU_CLOSED) {
				initContextMenu();
				ctx_menu_mode = CONTEXT_MENU_OPENING;
			}
		}

		// Mark entry
		if (pressed_buttons & SCE_CTRL_SQUARE) {
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				if (!fileListFindEntry(&mark_list, file_entry->name)) {
					FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
					memcpy(mark_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&mark_list, mark_entry, SORT_NONE);
				} else {
					fileListRemoveEntry(&mark_list, file_entry->name);
				}
			}
		}

		// Back
		if (pressed_buttons & SCE_CTRL_CANCEL) {
			fileListEmpty(&mark_list);
			dirUp();
			refreshFileList();
		}
	}

	// Handle
	if (pressed_buttons & SCE_CTRL_ENTER) {
		fileListEmpty(&mark_list);

		// Handle file or folder
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
		if (file_entry->is_folder) {
			if (strcmp(file_entry->name, DIR_UP) == 0) {
				dirUp();
			} else {
				if (dir_level == 0) {
					strcpy(cur_path, file_entry->name);
					memset(&mount_point_stat, 0, sizeof(SceIoStat));
					sceIoGetstat(file_entry->name, &mount_point_stat);
				} else {
					strcat(cur_path, file_entry->name);
				}

				dirLevelUp();
			}

			refreshFileList();
		} else {
			sprintf(cur_file, "%s%s", cur_path, file_entry->name);
			int type = handleFile(cur_file);

			// Archive mode
			if (type == FILE_TYPE_RAR || type == FILE_TYPE_7ZIP || type == FILE_TYPE_ZIP) {
				is_in_archive = 1;
				dir_level_archive = dir_level;

				sprintf(archive_path, "%s%s", cur_path, file_entry->name);

				strcat(cur_path, file_entry->name);
				addEndSlash(cur_path);

				dirLevelUp();
				refreshFileList();
			}
		}
	}
}

int shellMain() {
	// Position
	memset(base_pos_list, 0, sizeof(base_pos_list));
	memset(rel_pos_list, 0, sizeof(rel_pos_list));

	// Paths
	memset(cur_file, 0, sizeof(cur_file));
	memset(cur_path, 0, sizeof(cur_path));
	memset(copy_path, 0, sizeof(copy_path));
	memset(archive_path, 0, sizeof(archive_path));

	// Home
	strcpy(cur_path, HOME_PATH);

	// Reset file lists
	resetFileLists();

	while (1) {
		readPad();

		int refresh = 0;

		// Control
		if (dialog_step == DIALOG_STEP_NONE) {
			if (ctx_menu_mode != CONTEXT_MENU_CLOSED) {
				contextMenuCtrl();
			} else {
				fileBrowserMenuCtrl();
			}
		} else {
			refresh = dialogSteps();
		}

		if (refresh) {
			// Refresh lists
			refreshFileList();
			refreshMarkList();
			refreshCopyList();
		}

		// Start drawing
		START_DRAWING();

		// Draw shell info
		drawShellInfo(cur_path);

		// Draw scroll bar
		drawScrollBar(base_pos, file_list.length);

		// Draw
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos);

		int i;
		for (i = 0; i < MAX_ENTRIES && (base_pos + i) < file_list.length; i++) {
			uint32_t color = WHITE;

			if (file_entry->is_folder)
				color = CYAN;
/*
			if (file_entry->type == FILE_TYPE_RAR || file_entry->type == FILE_TYPE_7ZIP || file_entry->type == FILE_TYPE_ZIP) {
				color = YELLOW;
			}
*/
			if (i == rel_pos)
				color = GREEN;

			float y = START_Y + (i * FONT_Y_SPACE);

			// Marked
			if (fileListFindEntry(&mark_list, file_entry->name))
				vita2d_draw_rectangle(SHELL_MARGIN_X, y + 3.0f, MARK_WIDTH, FONT_Y_SPACE, COLOR_ALPHA(AZURE, 0x4F));

			// File name
			vita2d_pgf_draw_text(font, SHELL_MARGIN_X, y, color, FONT_SIZE, file_entry->name);

			// File information
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				// Folder/Size
				char size_string[16];
				getSizeString(size_string, file_entry->size);

				char *str = file_entry->is_folder ? language_container[FOLDER] : size_string;

				vita2d_pgf_draw_text(font, ALIGN_LEFT(680.0f, vita2d_pgf_text_width(font, FONT_SIZE, str)), y, color, FONT_SIZE, str);

				// Date
				char date_string[16];
				getDateString(date_string, date_format, &file_entry->time);

				char time_string[24];
				getTimeString(time_string, time_format, &file_entry->time);

				char string[64];
				sprintf(string, "%s %s", date_string, time_string);

				vita2d_pgf_draw_text(font, ALIGN_LEFT(SCREEN_WIDTH - SHELL_MARGIN_X, vita2d_pgf_text_width(font, FONT_SIZE, string)), y, color, FONT_SIZE, string);
			}

			// Next
			file_entry = file_entry->next;
		}

		// Draw context menu
		drawContextMenu();

		// End drawing
		END_DRAWING();
	}

	// Empty lists
	fileListEmpty(&copy_list);
	fileListEmpty(&mark_list);
	fileListEmpty(&file_list);

	return 0;
}

void initShell() {
	int i;
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility != VISIBILITY_UNUSED)
			ctx_menu_max_width = MAX(ctx_menu_max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_entries[i].name]));

		if (menu_entries[i].name == MARK_ALL) {
			menu_entries[i].name = UNMARK_ALL;
			i--;
		}
	}

	ctx_menu_max_width += 2.0f * CONTEXT_MENU_MARGIN;
	ctx_menu_max_width = MAX(ctx_menu_max_width, CONTEXT_MENU_MIN_WIDTH);
}

#ifdef RELEASE

void showSplashScreen() {
	vita2d_texture *splash = vita2d_load_PNG_buffer(splashscreen);

	int fade_mode = 0, fade_alpha = NOALPHA;

	while (fade_mode < 2) {
		// Fade
		if (!fade_mode) {
			if (fade_alpha > 0) {
				fade_alpha -= 3;
			} else {
				fade_mode++;
				sceKernelDelayThread(2 * 1000 * 1000);
			}
		} else {
			if (fade_alpha < NOALPHA) {
				fade_alpha += 3;
			} else {
				fade_mode++;
			}
		}

		// Start drawing
		START_DRAWING();

		// Draw splashscreen and black alpha rectangle
		vita2d_draw_texture(splash, 0, 0);
		vita2d_draw_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_ALPHA(0, fade_alpha));

		// End drawing
		END_DRAWING();
	}

	vita2d_free_texture(splash);
}

#endif

void getNetInfo() {
	static char memory[16 * 1024];

	SceNetInitParam param;
	param.memory = memory;
	param.size = sizeof(memory);
	param.flags = 0;

	int net_init = sceNetInit(&param);
	int netctl_init = sceNetCtlInit();

	sceNetGetMacAddress(&mac, 0);

	SceNetCtlInfo info;
	if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) {
		strcpy(ip, "-");
	} else {
		strcpy(ip, info.ip_address);
	}

	if (netctl_init >= 0)
		sceNetCtlTerm();

	if (net_init >= 0)
		sceNetTerm();
}

//SceAppMgrUser_09899A08(3, string, 17);
//SceAppMgrUser_5E375921(mount_point) umount?
//0x84DE76C7: sceAppMgrSaveDataDataSave
//SceAppMgrUser_CECFC7CB("vs0:app/NPXS10015/eboot.bin", "NPXS10016", 0x2000000);

/*
	Callback list
	0x82833474: 0x82823BAB '.;..' - addhi      a4, a3, #0x2ac00							called by 0x82404960
	0x82833478: 0x828243F7 '.C..' - addhi      v1, a3, #-0x23fffffd						called by 0x8240489C
	0x8283347C: 0x828247FB '.G..' - addhi      v1, a3, #0x3ec0000						called by 0x82404A3E
	0x82833480: 0x82824D63 'cM..' - addhi      v1, a3, #0x18c0
	0x82833484: 0x82825155 'UQ..' - addhi      v2, a3, #0x40000015						receive from avmedia service? ;) calling sub_82824EEC to receive file buffer
	0x82833488: 0x828257AF '.W..' - addhi      v2, a3, #0x2bc0000						called by 0x82406CAA?
	0x8283348C: 0x82825FA5 '._..' - addhi      v2, a3, #0x294
	0x82833490: 0x828266A5 '.f..' - addhi      v3, a3, #0xa500000						called by 0x82406D04?
	0x82833494: 0x82826F43 'Co..' - addhi      v3, a3, #0x10c
	0x82833498: 0x828273CD '.s..' - addhi      v4, a3, #0x34000003						called by 0x82406D5E?
	0x8283349C: 0x82827A47 'Gz..' - addhi      v4, a3, #0x47000
	0x828334A0: 0x82827A8F '.z..' - addhi      v4, a3, #0x8f000
	0x828334A4: 0x82827B43 'C{..' - addhi      v4, a3, #0x10c00
	0x828334A8: 0x82827B47 'G{..' - addhi      v4, a3, #0x11c00
	0x828334AC: 0x82827E4D 'M~..' - addhi      v4, a3, #0x4d0
	0x828334B0: 0x828227E9 '.'..' - addhi      a3, a3, #0x3a40000
	0x828334B4: 0x8282283D '=(..' - addhi      a3, a3, #0x3d0000
*/

#define SCE_PHOTO_EXPORT_MAX_FS_PATH (1024)
#define SCE_PHOTO_EXPORT_MAX_PHOTO_TITLE_LENGTH (64)
#define SCE_PHOTO_EXPORT_MAX_PHOTO_TITLE_SIZE (SCE_PHOTO_EXPORT_MAX_PHOTO_TITLE_LENGTH * 4)
#define SCE_PHOTO_EXPORT_MAX_MEMBLOCK_SIZE (64 * 1024)

typedef struct ScePhotoExportParam {
	SceUInt32 version;
	const SceChar8 *photoTitle;
	const SceChar8 *gameTitle;
	const SceChar8 *gameComment;
	SceChar8 reserved[32];
} ScePhotoExportParam;

typedef SceBool (*ScePhotoExportCancelFunc)(void*);

uint32_t shellsvc_addr = 0, photoexport_addr = 0;
uint32_t ori_shellsvc_addr = 0;

/*
	Knowledge:
	- Trying to receive 0x10 (size - 4) instead of 0x14 will make result 0x10 too
	- Trying to receive a buffer bigger than it is send (size + 4) will return 0x80028223
	- Tryint to receive a second time with same isze will result: 0x80028223
	- You can split a packet! Receive size - 4 and then 4 will work. Receive size - 4 and then 8 won't work
*/

int sceKernelSendMsgPipePatched(SceUID uid, void *message, unsigned int size, int unk1, int *unk2, unsigned int *timeout) {
	char string[128];
	sprintf(string, "cache0:/dump/send_0x%08X_0x%X.bin", (unsigned int)message, size);
	WriteFile(string, message, size);

	static char buffer[0x4000];
	memcpy(buffer, message, size);

	if (size == 0x40) {
		// trying to make pointers invalid
//		*(uint32_t *)((uint32_t)buffer + 0x38) = 0x12345678;
//		*(uint32_t *)((uint32_t)buffer + 0x3C) = 0x12345678;
	} else {
//		*(uint32_t *)((uint32_t)buffer + 0x00) = 3; // using an other value will crash
//		*(uint32_t *)((uint32_t)buffer + 0x04) = 8;
	}

	int res = sceKernelSendMsgPipe(uid, buffer, size, unk1, unk2, timeout);

	debugPrintf("%s 0x%08X 0x%08X: 0x%08X\n", __FUNCTION__, message, size, res);

	return res;
}

int sceKernelTrySendMsgPipePatched(SceUID uid, void *message, unsigned int size, int unk1, void *unk2) {
	static char buffer[0x4000];
	memcpy(buffer, message, size);

	if (size == 0x20) {
		if (strcmp(buffer + 0x1C, "Expo") == 0) {
			//*(uint32_t *)(buffer + 0x14) = -1;
		} else {
			//*(uint32_t *)(buffer + 0x00) = 0x40000; // Changing output size
			//*(uint32_t *)(buffer + 0x10) = 0x4000;
		}

		//memset(buffer, -1, size);
	}

	char string[128];
	sprintf(string, "cache0:/dump/try_send_0x%08X_0x%X.bin", (unsigned int)message, size);
	WriteFile(string, message, size);

	int res = sceKernelTrySendMsgPipe(uid, buffer, size, unk1, unk2);

	debugPrintf("%s 0x%08X 0x%08X: 0x%08X\n", __FUNCTION__, message, size, res);

	return res;
}

int sceKernelTryReceiveMsgPipePatched(SceUID uid, void *message, unsigned int size, int unk1, int *result) {
	int res = sceKernelTryReceiveMsgPipe(uid, message, size, unk1, result);

	char string[128];
	sprintf(string, "cache0:/dump/recv_0x%08X_0x%X.bin", (unsigned int)message, size);
	WriteFile(string, message, size);

	debugPrintf("%s 0x%08X 0x%08X: 0x%08X, 0x%08X\n", __FUNCTION__, message, size, res, *result);

	return res;
}

/*
int sub_8282C578(void *a1, void *message, int *result, unsigned int size) {
	if (size == 0) {
		if (result) {
			*result = 0;
			return 0;
		}
	}

	int res = sceKernelTryReceiveMsgPipe(*(uint32_t *)a1, message, size, 1, result);

	if (res == SCE_KERNEL_ERROR_MSG_PIPE_EMPTY || res == SCE_KERNEL_ERROR_MSG_PIPE_DELETED)
		return 0x80020511;

	return res;
}
*/
int (* SceIpmi_B282B430)(void *a1, char *a2, void *a3, void *a4);
/*
int (* sub_82823BAA)(void *packet);
int (* sub_828243F6)(void *packet, int a2, int a3, void *a4);
int (* sub_828247FA)(void *packet);
int (* sub_828257AE)(void *packet, int cmd, void *input, void *a4, void *output, int a6);
int (* sub_828266A4)(void *packet, int cmd, void *input, void *a4, void *output, int a6);
int (* sub_828273CC)(void *packet, int cmd, void *input, void *a4, void *output, int a6);

int sub_82823BAA_patched(void *packet) {
	debugPrintf("0) %s\n", __FUNCTION__);
	return sub_82823BAA(packet);
}

int sub_828243F6_patched(void *packet, int a2, int a3, void *a4) {
	debugPrintf("1) %s\n", __FUNCTION__);
	return sub_828243F6(packet, a2, a3, a4);
}

int sub_828247FA_patched(void *packet) {
	debugPrintf("2) %s\n", __FUNCTION__);
	return sub_828247FA(packet);
}

int sub_828257AE_patched(void *packet, int cmd, void *input, void *a4, void *output, int a6) {
	debugPrintf("5) %s\n", __FUNCTION__);
	return sub_828257AE(packet, cmd, input, a4, output, a6);
}

int sub_828266A4_patched(void *packet, int cmd, void *input, void *a4, void *output, int a6) {
	debugPrintf("7) %s\n", __FUNCTION__);
	return sub_828266A4(packet, cmd, input, a4, output, a6);
}

int sub_828273CC_patched(void *packet, int cmd, void *input, void *a4, void *output, int a6) {
	debugPrintf("9) %s\n", __FUNCTION__);
	return sub_828273CC(packet, cmd, input, a4, output, a6);
}
*/
int SceIpmi_B282B430_Patched(void *a1, char *a2, void *a3, void *a4) {
	int res = SceIpmi_B282B430(a1, a2, a3, a4);

	uint32_t val = *(uint32_t *)a1;

	uint32_t *func_list = (uint32_t *)(shellsvc_addr + (0x82833474 - 0x82820FE0));

	*(uint32_t *)val = (uint32_t)func_list;

	uvl_unlock_mem();
/*
	sub_82823BAA = (void *)func_list[0];
	func_list[0] = (uint32_t)sub_82823BAA_patched;

	sub_828243F6 = (void *)func_list[1];
	func_list[1] = (uint32_t)sub_828243F6_patched;

	sub_828247FA = (void *)func_list[2];
	func_list[2] = (uint32_t)sub_828247FA_patched;

	sub_828257AE = (void *)func_list[5];
	func_list[5] = (uint32_t)sub_828257AE_patched;

	sub_828266A4 = (void *)func_list[7];
	func_list[7] = (uint32_t)sub_828266A4_patched;

	sub_828273CC = (void *)func_list[9];
	func_list[9] = (uint32_t)sub_828273CC_patched;
*/
	func_list[3] = 0;
	func_list[4] = 0;
	func_list[6] = 0;
	func_list[8] = 0;
	func_list[10] = 0;

	uvl_lock_mem();
	uvl_flush_icache((void *)(shellsvc_addr + (0x82833474 - 0x82820FE0)), 18 * 4);

	return res;
}

void hack() {
	removePath("cache0:/dump", NULL, 0, NULL);
	sceIoMkdir("cache0:/dump", 0777);

	sceSysmoduleLoadModule(SCE_SYSMODULE_PHOTO_EXPORT);

	findModuleByName("SceShellSvc", &ori_shellsvc_addr, NULL);

	duplicateModule("SceShellSvc", &shellsvc_addr, NULL);

	duplicateModule("ScePhotoExport", &photoexport_addr, NULL);

	uvl_unlock_mem();

	// Patch
	uint32_t i;
	for (i = 0; i < 18; i++) {
		uint32_t offset = *(uint32_t *)(shellsvc_addr + i * 4 + (0x82833474 - 0x82820FE0));
		*(uint32_t *)(shellsvc_addr + i * 4 + (0x82833474 - 0x82820FE0)) = offset - ori_shellsvc_addr + shellsvc_addr;
	}

	uvl_lock_mem();
	uvl_flush_icache((void *)(shellsvc_addr + (0x82833474 - 0x82820FE0)), 18 * 4);

	HIJACK_STUB(photoexport_addr + (0x824076B8 - 0x824045D0), SceIpmi_B282B430_Patched, SceIpmi_B282B430);

	int (* x)();
	HIJACK_STUB(shellsvc_addr + (0x8283061C - 0x82820FE0), sceKernelSendMsgPipePatched, x);
	HIJACK_STUB(shellsvc_addr + (0x8283085C - 0x82820FE0), sceKernelTrySendMsgPipePatched, x);
	HIJACK_STUB(shellsvc_addr + (0x8283071C - 0x82820FE0), sceKernelTryReceiveMsgPipePatched, x);

	// Function test
	SceInt32 (* scePhotoExportFromFile)(const SceChar8 *photodataPath, const ScePhotoExportParam *param, void *workMemory, ScePhotoExportCancelFunc cancelFunc, void *userdata, SceChar8 *exportedPath, SceInt32 exportedPathLength);

	scePhotoExportFromFile = (void *)photoexport_addr + (0xDF6 | 0x1);

	ScePhotoExportParam	exportParam;
	memset(&exportParam, 0, sizeof(ScePhotoExportParam));
	exportParam.photoTitle = (SceChar8 *)"THIS_IS_THE_PHOTO_TITLE";
	exportParam.gameTitle = (SceChar8 *)"THIS_IS_THE_GAME_TITLE";
	exportParam.gameComment = (SceChar8 *)"THIS_IS_THE_GAME_COMMENT";

	static char	g_workMemory[SCE_PHOTO_EXPORT_MAX_MEMBLOCK_SIZE];
	static char	g_exportPath[SCE_PHOTO_EXPORT_MAX_FS_PATH];

	int res = scePhotoExportFromFile((SceChar8 *)"cache0:/test.jpeg", &exportParam, g_workMemory, NULL, NULL, (SceChar8 *)g_exportPath, SCE_PHOTO_EXPORT_MAX_FS_PATH);
	debugPrintf("g_exportPath: %s\n", g_exportPath);

	// sceSysmoduleUnloadModule(SCE_SYSMODULE_PHOTO_EXPORT);
}

void freePreviousVitaShell() {
	sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr((void *)extractFunctionStub((uint32_t)&sceKernelBacktrace), 0));
	sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr((void *)extractFunctionStub((uint32_t)&sceKernelBacktraceSelf), 0));
}

int vitashell_thread(SceSize args, void *argp) {
#ifndef RELEASE
	// sceIoRemove("cache0:vitashell_log.txt");
#endif

	// Free .text and .data segment of previous VitaShell
	freePreviousVitaShell();

	// Init VitaShell
	VitaShellInit();

	// Set up nid table
	// setupNidTable();

	// Get UVL address, backup and patch it
	getUVLTextAddr();
	backupUVL();
	PatchUVL();

	// Get net info
	getNetInfo();

	// Load language
	loadLanguage(language);

	// Show splash screen
#ifdef RELEASE
	showSplashScreen();
#endif

	// hack();
	// loadDumpModules();

	// Main
	initShell();
	shellMain();

	return sceKernelExitDeleteThread(0);
}

int main(int argc, const char *argv[]) {
	// Start app with bigger stack
	SceUID thid = sceKernelCreateThread("VitaShell_main_thread", (SceKernelThreadEntry)vitashell_thread, 0x10000100, 1 * 1024 * 1024, 0, 0x70000, NULL);
	if (thid >= 0) {
		sceKernelStartThread(thid, argc, argv);
		sceKernelWaitThreadEnd(thid, NULL, NULL);
	}

	return 0;
}
