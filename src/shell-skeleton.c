#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h> // termios, TCSANOW, ECHO, ICANON
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

const char *sysname = "mishell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n",
		   command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");

	for (i = 0; i < 3; i++) {
		printf("\t\t%d: %s\n", i,
			   command->redirects[i] ? command->redirects[i] : "N/A");
	}

	printf("\tArguments (%d):\n", command->arg_count);

	for (i = 0; i < command->arg_count; ++i) {
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	}

	if (command->next) {
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
	if (command->arg_count) {
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}

	for (int i = 0; i < 3; ++i) {
		if (command->redirects[i])
			free(command->redirects[i]);
	}

	if (command->next) {
		free_command(command->next);
		command->next = NULL;
	}

	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt(void) {
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
*/
int parse_command(char *buf, struct command_t *command) {
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);

	// trim left whitespace
	while (len > 0 && strchr(splitters, buf[0]) != NULL) {
		buf++;
		len--;
	}

	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL) {
		// trim right whitespace
		buf[--len] = 0;
	}

	// auto-complete
	if (len > 0 && buf[len - 1] == '?') {
		command->auto_complete = true;
	}

	// background
	if (len > 0 && buf[len - 1] == '&') {
		command->background = true;
	}

	char *pch = strtok(buf, splitters);
	if (pch == NULL) {
		command->name = (char *)malloc(1);
		command->name[0] = 0;
	} else {
		command->name = (char *)malloc(strlen(pch) + 1);
		strcpy(command->name, pch);
	}

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1) {
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// trim left whitespace
		while (len > 0 && strchr(splitters, arg[0]) != NULL) {
			arg++;
			len--;
		}

		// trim right whitespace
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) {
			arg[--len] = 0;
		}

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// piping to another command
		if (strcmp(arg, "|") == 0) {
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0) {
			// handled before
			continue;
		}

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<') {
			redirect_index = 0;
		}

		if (arg[0] == '>') {
			if (len > 1 && arg[1] == '>') {
				redirect_index = 2;
				arg++;
				len--;
			} else {
				redirect_index = 1;
			}
		}

		if (redirect_index != -1) {
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 &&
			((arg[0] == '"' && arg[len - 1] == '"') ||
			 (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}

		command->args =
			(char **)realloc(command->args, sizeof(char *) * (arg_index + 1));

		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;

	// increase args size by 2
	command->args = (char **)realloc(
		command->args, sizeof(char *) * (command->arg_count += 2));

	// shift everything forward by 1
	for (int i = command->arg_count - 2; i > 0; --i) {
		command->args[i] = command->args[i - 1];
	}

	// set args[0] as a copy of name
	command->args[0] = strdup(command->name);

	// set args[arg_count-1] (last) to NULL
	command->args[command->arg_count - 1] = NULL;

	return 0;
}

void prompt_backspace(void) {
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
	size_t index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &=
		~(ICANON |
		  ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	show_prompt();
	buf[0] = 0;

	while (1) {
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		// handle tab
		if (c == 9) {
			buf[index++] = '?'; // autocomplete
			break;
		}

		// handle backspace
		if (c == 127) {
			if (index > 0) {
				prompt_backspace();
				index--;
			}
			continue;
		}

		if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
			continue;
		}

		// up arrow
		if (c == 65) {
			while (index > 0) {
				prompt_backspace();
				index--;
			}

			char tmpbuf[4096];
			printf("%s", oldbuf);
			strcpy(tmpbuf, buf);
			strcpy(buf, oldbuf);
			strcpy(oldbuf, tmpbuf);
			index += strlen(buf);
			continue;
		}

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}

	// trim newline from the end
	if (index > 0 && buf[index - 1] == '\n') {
		index--;
	}

	// null terminate string
	buf[index++] = '\0';

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);

int main(void) {
	while (1) {
		struct command_t *command = malloc(sizeof(struct command_t));

		// set all bytes to 0
		memset(command, 0, sizeof(struct command_t));

		int code;
		code = prompt(command);
		if (code == EXIT) {
			break;
		}

		code = process_command(command);
		if (code == EXIT) {
			break;
		}

		free_command(command);
	}

	printf("\n");
	return 0;
}
void removeSpaces(char *str) {
	int count = 0;
	for (int i = 0; str[i]; i++)
		if (str[i] != ' ' && str[i] != '\t')
			str[count++] = str[i]; // here count is incremented
	str[count] = '\0';
}
void lineCount(int counts[4][4], char *fileName) {
	FILE *fp = fopen(fileName, "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	char *ext = strrchr(fileName, '.');

	// Check if file exists
	if (fp == NULL) {
		printf("Could not open file %s", fileName);
		return;
	}
	bool command = false;
	while ((read = getline(&line, &len, fp)) != -1) {
		removeSpaces(line);
		if (ext) {
			if (strcmp(ext, ".py") == 0) { //PYTHON.
				if (strlen(line) == 1) {
					counts[0][1] += 1;
				} else if (line[0] == '\'' && line[1] == '\'' &&
						   line[2] == '\'') {
					command = true;
					counts[0][2] += 1;
					if (strstr(line, "\'\'\'") != NULL) {
						command = false;
					}
				} else if (command) {
					counts[0][2] += 1;
					if (strstr(line, "\'\'\'") != NULL) {
						command = false;
					}
				} else if (line[0] == '\"' && line[1] == '\"' &&
						   line[2] == '\"') {
					command = true;
					counts[0][2] += 1;
					if (strstr(line, "\"\"\"") != NULL) {
						command = false;
					}
				} else if (command) {
					counts[0][2] += 1;
					if (strstr(line, "\"\"\"") != NULL) {
						command = false;
					}
				} else if (line[0] == '#') {
					counts[0][2] += 1;
				} else {
					counts[0][0] += 1;
				}

			} else if (strcmp(ext, ".cpp") == 0) { //C++.
				if (strlen(line) == 1) {
					counts[1][1] += 1;
				} else if (line[0] == '/' && line[1] == '*') {
					command = true;
					counts[1][2] += 1;
					if (strstr(line, "*/") != NULL) {
						command = false;
					}
				} else if (command) {
					counts[1][2] += 1;
					if (strstr(line, "*/") != NULL) {
						command = false;
					}
				} else if (line[0] == '/' && line[1] == '/') {
					counts[1][2] += 1;
				} else {
					counts[1][0] += 1;
				}

			} else if (strcmp(ext, ".c") == 0) { //C.
				if (strlen(line) == 1) {
					counts[2][1] += 1;
				} else if (line[0] == '/' && line[1] == '*') {
					command = true;
					counts[2][2] += 1;
					if (strstr(line, "*/") != NULL) {
						command = false;
					}
				} else if (command) {
					counts[2][2] += 1;
					if (strstr(line, "*/") != NULL) {
						command = false;
					}
				} else if (line[0] == '/' && line[1] == '/') {
					counts[2][2] += 1;
				} else {
					counts[2][0] += 1;
				}
			} else {
				if (strlen(line) == 1) {
					counts[3][1] += 1;
				} else {
					counts[3][0] += 1;
				}
			}
		} else {
			if (strlen(line) == 1) {
				counts[3][1] += 1;
			} else {
				counts[3][0] += 1;
			}
		}
	}

	// Close the file
	fclose(fp);
	return;
}
void listFiles(int *ignored_files, int *processed_files, char *dirname,
			   int counts[4][4]) {
	DIR *dir = opendir(dirname);
	if (dir == NULL) {
		printf("No folder found!\n");
		return;
	}
	struct dirent *ent;
	ent = readdir(dir);
	while (ent != NULL) {
		char name[256];
		strcpy(name, ent->d_name);
		char *ext = strrchr(name, '.');
		if (ent->d_type == 4 && strcmp(name, ".") != 0 &&
			strcmp(name, "..") != 0 && name[0] != '.') {
			char path[512];
			sprintf(path, "%s/%s", dirname, ent->d_name);
			listFiles(ignored_files, processed_files, path, counts);
		} else if (name[0] != '.') {
			*processed_files += 1;
			if (ext) {
				if (strcmp(ext, ".py") == 0) {
					counts[0][3] += 1;
				} else if (strcmp(ext, ".cpp") == 0) {
					counts[1][3] += 1;
				} else if (strcmp(ext, ".c") == 0) {
					counts[2][3] += 1;
				} else {
					counts[3][3] += 1;
				}
			} else {
				counts[3][3] += 1;
			}
			char fileName[512];
			sprintf(fileName, "%s/%s", dirname, ent->d_name);
			lineCount(counts, fileName);
		} else {
			if (strcmp(name, "..") != 0 && strcmp(name, ".") != 0) {
				*ignored_files += 1;
			}
		}
		ent = readdir(dir);
	}
	closedir(dir);
}
int process_command(struct command_t *command) {
	int r;

	if (strcmp(command->name, "") == 0) {
		return SUCCESS;
	}

	if (strcmp(command->name, "exit") == 0) {
		return EXIT;
	}

	if (strcmp(command->name, "cd") == 0) {
		if (command->arg_count > 0) {
			r = chdir(command->args[1]); // Changed to first argument
			if (r == -1) {
				printf("-%s: %s: %s\n", sysname, command->name,
					   strerror(errno));
			}

			char cwdpath[256];
			char *home_dir = getenv("HOME");
			char full_path[256];
			sprintf(full_path, "%s/%s", home_dir, "cdhistory.txt");
			FILE *cdhistory = fopen(full_path, "r+");
			char history[10][256];
			char new_history[11][256];
			int i = 0;

			if (cdhistory != NULL) {
				while (fscanf(cdhistory, "%s\n", history[i]) !=
					   EOF) { //getting cd history from saved file into an array
					i++;
				}
				fclose(cdhistory);
			}

			getcwd(cwdpath, sizeof(cwdpath));
			int curr_index = 0;
			int ex_index = 0;
			while (ex_index < i) {
				if (strcmp(history[ex_index], cwdpath) != 0) {
					strcpy(
						new_history[curr_index],
						history[ex_index]); //copying unique paths into new array
					curr_index++; //and if the new path already exists in history
				} //we don't copy it so we can add it to the end of the file
				ex_index++; //making it the latest unique path entry
			}
			strcpy(new_history[curr_index], cwdpath);
			curr_index++;
			cdhistory = fopen(full_path, "w");
			if (curr_index ==
				11) { //checking if we exceeded the 10 unique path limit
				for (int c = 0; c < 10; c++) { //then take action accordingly
					//printf("%s\n", new_history[c+1]);
					fprintf(cdhistory, "%s\n",
							new_history[c + 1]); //deleting the oldest path
				}
			} else {
				for (int c = 0; c < curr_index; c++) {
					//printf("%s\n", new_history[c]);
					fprintf(cdhistory, "%s\n", new_history[c]);
				}
			}
			fclose(cdhistory);
			return SUCCESS;
		}
	}

	if (strcmp(command->name, "cdh") == 0) {
		char *home_dir = getenv("HOME");
		char full_path[256];
		sprintf(full_path, "%s/%s", home_dir, "cdhistory.txt");
		char history[10][256];
		FILE *cdhistory = fopen(full_path, "r");
		char new_history[10][256];

		if (cdhistory == NULL) {
			return SUCCESS;
		}

		char s = 'a';
		int i = 0;
		while (fscanf(cdhistory, "%s\n", history[i]) !=
			   EOF) { //getting history into array
			i++;
		}
		fclose(cdhistory);
		int j = i;
		for (int k = 0; k < j;
			 k++) { //printing history as seen in the given example
			printf("%c  %d)  %s\n", s + i - 1, i, history[k]);
			i--;
		}

		char choice = 'a';
		int choiceNum = 0;
		printf("Select directory by letter or number: ");
		scanf("%c", &choice);

		if (isdigit(choice) !=
			0) { //converting choice into int to be able to index array
			choiceNum = choice - '0';
		} else {
			choiceNum = choice - 'a' + 1;
		}

		choiceNum = j - choiceNum;
		r = chdir(history[choiceNum]); //changing dir
		if (r == -1) {
			printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
		}

		int curr_index = 0;
		int ex_index = 0;
		while (ex_index <
			   j) //refreshing history.txt according to the latest changed dir
		{
			if (strcmp(history[ex_index], history[choiceNum]) != 0) {
				strcpy(new_history[curr_index], history[ex_index]);
				curr_index++;
			}
			ex_index++;
		}
		strcpy(new_history[curr_index], history[choiceNum]);
		curr_index++;

		cdhistory = fopen(full_path, "w");
		for (int c = 0; c < curr_index; c++) {
			//printf("%s\n", new_history[c]);
			fprintf(cdhistory, "%s\n", new_history[c]);
		}
		fclose(cdhistory);

		return SUCCESS;
	}

	if (strcmp(command->name, "roll") == 0) {
		if (command->arg_count != 3) {
			fprintf(stderr, "Wrong argument count!");
			return SUCCESS;
		} else {
			int rollCount = 1;
			int diceSize = 1;
			char rollArgs[10];
			strcpy(rollArgs, command->args[1]);
			char *ptr;
			int first_num = strtol(rollArgs, &ptr, 10);
			if (first_num != 0 && ptr[0] == 'd') {
				char *rest;
				rollCount = first_num;
				diceSize = strtol(&ptr[1], &rest, 10);
				int total = 0;
				int nums[rollCount];
				for (int i = 0; i < rollCount; i++) {
					nums[i] = (rand() % diceSize) + 1;
					total += nums[i];
				}
				printf("Rolled %d (", total);
				for (int i = 0; i < rollCount - 1; i++) {
					printf("%d + ", nums[i]);
				}
				printf("%d)\n", nums[rollCount - 1]);
				return SUCCESS;
			} else if (ptr[0] == 'd') {
				char *rest;
				diceSize = strtol(&ptr[1], &rest, 10);
				printf("Rolled %d \n", (rand() % diceSize) + 1);
				return SUCCESS;
			} else {
				printf("Error in argument.\n");
				return SUCCESS;
			}
			printf("rollcount: %d, dicesize: %d\n", rollCount, diceSize);
		}
	}
	if (strcmp(command->name, "cloc") == 0) {
		char path[512];
		char cloc_path[1024];
		getcwd(path, sizeof(path));
		sprintf(cloc_path, "%s/%s", path, command->args[1]);
		int processed_files = 0;
		int ignored_files = 0;
		int counts[4][4] = {
			{ 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }
		};
		//2D counts array has 4 int-arr for python,c++,c and txt(all other formats.)
		// counts[x][0] = total #of lines of code.
		// counts[x][1] = total #of blank lines
		// counts[x][2] = total #of command lines.
		// counts[x][3] = total #of files for that type.
		listFiles(&ignored_files, &processed_files, cloc_path, counts);
		printf("Total Number of files found: %d\n",
			   ignored_files + processed_files);
		printf("Number of ignored files: %d\n", ignored_files);
		printf("Number of processed files: %d\n", processed_files);
		printf("Python; %d files, %d blank, %d command, %d code lines.\n",
			   counts[0][3], counts[0][1], counts[0][2], counts[0][0]);
		printf("Cpp;    %d files, %d blank, %d command, %d code lines.\n",
			   counts[1][3], counts[1][1], counts[1][2], counts[1][0]);
		printf("C;      %d files, %d blank, %d command, %d code lines.\n",
			   counts[2][3], counts[2][1], counts[2][2], counts[2][0]);
		printf("Txt;    %d files, %d blank, %d command, %d code lines.\n",
			   counts[3][3], counts[3][1], counts[3][2], counts[3][0]);
		int total_counts[4] = { 0, 0, 0, 0 };
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++) {
				total_counts[j] += counts[i][j];
			}
		}
		printf("Total;  %d files, %d blank, %d command, %d code lines.\n",
			   total_counts[3], total_counts[1], total_counts[2],
			   total_counts[0]);
		return SUCCESS;
	}

	if (strcmp(command->name, "rename") == 0 ||
		strcmp(command->name, "mvsf") == 0) {
		int status = 0;
		char path[256];
		char oldfilename[512];
		char newfilename[512];

		getcwd(path, sizeof(path));
		sprintf(oldfilename, "%s/%s", path, command->args[1]);
		sprintf(newfilename, "%s/%s", path, command->args[2]);

		if (command->arg_count != 4) {
			fprintf(
				stderr,
				"Wrong arguments!\nUsage for mvsf(move to subfolder): mvsf <old file name> <new file namepath>\nUsage for rename: rename <old file name> <new file name>\n");
			return SUCCESS;
		} else {
			status = rename(oldfilename, newfilename);
		}

		if (status == 0) {
			printf("File renamed/moved successfully.\n");
			return SUCCESS;
		} else {
			printf("Failed to rename/move the file.\n");
			return SUCCESS;
		}
	}
	if (strcmp(command->name, "searchwords") == 0) {
		char path[256];
		char file[512];
		char searched_word[256];
		int word_count = 0;
		getcwd(path, sizeof(path));
		sprintf(file, "%s/%s", path, command->args[1]);
		sprintf(searched_word, "%s", command->args[2]);
		if (command->arg_count != 4) {
			fprintf(
				stderr,
				"Wrong arguments! Usage for searchwords: searchwords <file name> <searched word>\n.");
			return SUCCESS;
		}
		FILE *search_file = fopen(file, "r");
		char word[1024];
		if (search_file == NULL) {
			printf("Couldn't opened the file.\n");
			return SUCCESS;
		}
		while (fscanf(search_file, "%1023s", word) == 1) {
			//puts(word);
			if (strstr(word, searched_word) != 0) {
				word_count += 1;
			}
		}
		fclose(search_file);
		printf("%s found %d times in file %s\n", searched_word, word_count,
			   command->args[1]);
		return SUCCESS;
	}
	if (strcmp(command->name, "psvis") == 0) {
		if (command->arg_count != 4) {
			printf("Wrong arguments! Usage: psvis <pid> <png name>\n");
			return SUCCESS;
		}
		int root_pid = atoi(command->args[1]);
		char command_exec[512];
		char word[256];
		int count = 0;
		system(
			"sudo -S dmesg > deneme.txt"); //getting kernel modules info before inserting our module
		system(
			"wc -l deneme.txt > den.txt"); //getting line count of dmesg for later use of parsing
		FILE *count_file = fopen("den.txt", "r");
		fscanf(count_file, "%s%*[^\n]",
			   word); //getting line count value to word variable
		fclose(count_file);
		count = atoi(word);
		kill(root_pid, 0);
		if (errno ==
			ESRCH) { //checking if the given pid exists to prevent module crash
			printf("Please enter a valid PID!\n");
			return SUCCESS;
		}
		sprintf(command_exec, "sudo -S insmod mymodule.ko pid=%d", root_pid);
		system(command_exec);
		system("sudo -S dmesg > deneme.txt");
		sprintf(command_exec, "sed '1,%dd' deneme.txt > deneme2.txt",
				count); //transfering new lines after insmod
		system(command_exec);
		system("sudo -S rmmod mymodule"); //removing module

		FILE *dmesg =
			fopen("deneme2.txt", "r"); //writing to .gv file to be drawn
		FILE *write_file = fopen("deneme3.gv", "w");
		fprintf(write_file, "digraph ProcessTree{\n");
		char line[1024];
		while (fgets(line, sizeof(line), dmesg)) {
			char *p = strchr(line, '"');
			fprintf(write_file, "%s", p);
		}
		fprintf(write_file, "}\n");
		fclose(dmesg);
		fclose(write_file);
		sprintf(
			command_exec, "dot -Tpng deneme3.gv -o %s",
			command->args[2]); //creating .png using graphviz with .gv source file
		if (system(command_exec) != 0) {
			printf("Please install graphviz packages!\n");
			return SUCCESS;
		}
		return SUCCESS;
		// Elimize saglik.
	}

	pid_t pid = fork();
	// child
	if (pid == 0) {
		/// This shows how to do exec with environ (but is not available on MacOs)
		// extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// TODO: do your own exec with path resolving using execv()
		// do so by replacing the execvp call below

		//execvp(command->name, command->args); // exec+args+path

		char *arr[command->arg_count + 1];

		for (int i = 0; i < command->arg_count; i++) {
			arr[i] = command->args[i];
		}

		arr[command->arg_count] = NULL;
		char *shellPath = getenv("PATH"); //getting PATH variable
		char pathcpy[1024];
		strcpy(
			pathcpy,
			shellPath); //copying the original variable because of strtok, so we can use the adress again after strtok destroys given address var.
		char path[1024];
		char e_path[1024];
		char *token = strtok(pathcpy, ":"); //tokenizing the path - first path

		while (true) {
			sprintf(path, "%s/%s", token, command->name);
			if (access(path, F_OK) == 0) {
				strcpy(
					e_path,
					path); //if found a working path for the given command, copy it to execute command in found path later
				break;
			}
			token = strtok(
				0, ":"); //continuing to tokenize other paths in PATH variable
			if (token == NULL) {
				break;
			}
		}

		if (command->redirects[0] !=
			NULL) { //handling three cases of redirecting
			int fd = open(command->redirects[0],
						  O_RDONLY); //opening the given file on read mode
			dup2(fd, STDIN_FILENO); //fd opened on desc. stdin
			execv(
				e_path,
				arr); //executing wanted command and since fd is opened on stdin, we have successfully directed stdin to file opened on fd
		} else if (command->redirects[1] != NULL) {
			int fd = creat(command->redirects[1],
						   0644); //creating/truncating given file
			dup2(fd, STDOUT_FILENO); //fd file desc. opened on file desc. stdout
			execv(
				e_path,
				arr); //executing wanted command and taking what is in on stdout desc. to file opened on fd desc.
			exit(0);
		} else if (command->redirects[2] != NULL) {
			int fd;
			if (access(command->redirects[2], F_OK) == 0) {
				fd = open(command->redirects[2],
						  O_WRONLY |
							  O_APPEND); //opening the existing file to append
			} else {
				fd = creat(
					command->redirects[2],
					0644); //handling the appending case where given file does not exist and needs to be created
			}
			dup2(fd, STDOUT_FILENO); //fd desc. opened on file desc. stdout
			execv(e_path,
				  arr); //executing command with redirected to wanted file
		}

		if (command
				->background) { //If background execute in newly created child process in background
			pid_t pidBack = fork();
			if (pidBack == 0) {
				execv(path, arr);
			}
			exit(0);
		}
		execv(e_path, arr);
		printf("-%s: %s: command not found\n", sysname, command->name);
		exit(0);

	} else {
		// TODO: implement background processes here
		wait(0); // wait for child process to finish
		return SUCCESS;
	}

	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}
