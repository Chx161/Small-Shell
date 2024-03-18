#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

// default: background & honored
int background_flag_signal = 1;
// Ctrl Z signal handler
// Change ^Z default action in parent process, i.e. the shell itself
// ^Z shouldn't stop shell process, but should toggle between two modes
void catchSIGTSTP()
{
	//Disable background command
	if (background_flag_signal == 1)
	{
		background_flag_signal = 0;
		char* message = "Entering foreground-only mode...\n";
		write(STDOUT_FILENO, message, 33);
	}
	//Enable background command
	else
	{
		background_flag_signal = 1;
		char* message_1 = "Exiting foreground-only mode...\n";
		write(STDOUT_FILENO, message_1, 32);
	}
}

// Reference: https://brennan.io/2015/01/16/write-a-shell-in-c/
// Get user input
char* lsh_read_line(void)
{
  char* line = NULL;
  ssize_t bufsize = 0; // have getline allocate a buffer for us
  getline(&line, &bufsize, stdin);
  return line;
}

// Reference: https://brennan.io/2015/01/16/write-a-shell-in-c/
// Tokenize line of user input into an array of strings
#define LSH_TOK_BUFSIZE 2048
#define LSH_TOK_DELIM " \t\r\n\a"
char** lsh_split_line(char *line)
{
  int bufsize = LSH_TOK_BUFSIZE, position = 0;
  char** tokens = malloc(bufsize * sizeof(char*));
  char* token;
	// Memory allocation error
  if (!tokens) {
    fprintf(stderr, "lsh: allocation error\n");
    exit(EXIT_FAILURE);
  }
	// Move token(ptr) string by string, store each string in tokens(array)
  token = strtok(line, LSH_TOK_DELIM);	//Return a ptr to str
  while (token != NULL)
	{
    tokens[position] = token;	// Store a string in tokens array
    position++;								// Increment index
    // if (position >= bufsize)		// Resize array if necessary
		// {
    //   bufsize += LSH_TOK_BUFSIZE;
    //   tokens = realloc(tokens, bufsize * sizeof(char*));
    //   if (!tokens) {
    //     fprintf(stderr, "lsh: allocation error\n");
    //     exit(EXIT_FAILURE);
    //   }
    // }
    token = strtok(NULL, LSH_TOK_DELIM);
  }
  tokens[position] = NULL;
  return tokens;
}

//Print out either the exit status or the terminating signal of the last FOREGROUND
//process (not both, processes killed by signals do not have exit statuses!)
//ran by your shell
void get_exit_status(int exit_status)
{
	//If exit normally
	if (WIFEXITED(exit_status))
		printf("Exit value %d\n", WEXITSTATUS(exit_status));
	//Else, terminated by signal
	else
		printf("Terminated by signal %d\n", WTERMSIG(exit_status));
}

//Check if string a starts with string b
bool startsWith(const char* a, const char* b)
{
	if (strncmp(a, b, strlen(b)) == 0)
		return 1;
	return 0;
}

// Builtin cd command
int lsh_cd(char **args, int* exit_status)
{
	char cwd[2048];
	int cwd_err = 0;	// dummy var
	//No args, change dir to HOME
  if (args[1] == NULL)
	{
		const char* home = getenv("HOME");
		// printf("HOME DIR = %s\n", home);
		chdir(home);
	}
	//Relative or absolute path specified
	else
	{
		if (chdir(args[1]) != 0)
			perror("lsh");
	}
  return 1;
}

// Builtin exit command
int lsh_exit(char** args, int* exit_status, int background_ids[])
{
	//wpid = waitpid(-1, status, WNOHANG) only works for reaping zombie process
	//If sleep 20 & is running in the background, it's not a zombie
	//wpid = waitpid(-1, status, WNOHANG) would return zero

	//If there are child processes but none of them is waiting to be noticed,
	//waitpid will block until one is. However, if the WNOHANG option was specified,
	//waitpid will return zero instead of blocking.
	//https://www.gnu.org/software/libc/manual/html_node/Process-Completion.html

	/* This does not work */
	// pid_t wpid;
	// int* status;
	// // Check for any unterminated process
	// while ((wpid = waitpid(-1, status, WNOHANG)) > 0)
	// {
	// 	printf("\nPARENT: sending SIGINT\n\n");
	// 	kill(wpid, SIGINT);
	// 	fflush(0);
	// }
	// Check the array storing child pids and kill any running background processes
	// User has access to shell, so the current foreground process is exit
	for (int i = 0; i < 1000; i++)
	{
		if (background_ids[i] != -5)
		{
			printf("PARENT: sending SIGINT. Child process %d killed\n", background_ids[i]);
			kill(background_ids[i], SIGINT);
			fflush(0);
		}
	}
  return 0;			// Refactor needed, function should return void
}

// Builtin status command
int lsh_status(char** args, int* exit_status)
{
	get_exit_status(*exit_status);
  return 1;
}

//Rediret I/O for background processes
void void_input()
{
	int fd = open("/dev/null", O_RDONLY);
	if (fd == -1) {perror("source open dev/null"); exit(1);}
	int result_in = dup2(fd, STDIN_FILENO);
	if (result_in == -1) { perror("source dup2()"); exit(2); }
}

void void_output()
{
	int fd = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) { perror("target open dev/null"); exit(1); }
	int result_out = dup2(fd, STDOUT_FILENO);
	if (result_out == -1) { perror("target dup2()"); exit(2); }
}

// If command is not builtin, fork off a copy child process and exec into a different one
// First check if input has & to decide if backgrounded or not
// Then deal with I/O rediretion
int lsh_launch(char **args, int* status, int background_ids[], struct sigaction* sa)
{
	// Set background process flag if needed
	// Get arg count for accessing the last arg
	int arg_count = 0;
	while (args[arg_count] != '\0')
		arg_count++;
	// printf("ls arg_count = %d\n", arg_count);
	int background_input = 0;
	if (strcmp(args[arg_count-1], "&") == 0)
		background_input = 1;

		// Create sigaction struct
		// Don't kill our shell, parent process should IGNORE ^C
		struct sigaction SIGINT_action = {0};
		SIGINT_action.sa_handler = SIG_IGN;
		sigfillset(&SIGINT_action.sa_mask);
		SIGINT_action.sa_flags = SA_RESTART;
		// Register signal with sigaction
		sigaction(SIGINT, &SIGINT_action, NULL);

	// Fork off a child process
  pid_t pid, wpid;
  pid = fork();
	// Child process
  if (pid == 0)
	{
		// Signal handler inherited to child process
		// Reset ^C to default for FOREGROUND child process, background child ignore ^C
		if (background_flag_signal == 0 || (background_flag_signal == 1 && background_input == 0))
		{
			SIGINT_action.sa_handler = SIG_DFL;
			sigaction(SIGINT, &SIGINT_action, NULL);
		}
		// Ignore ^Z in child process
		sa->sa_handler = SIG_IGN;
		sigaction(SIGTSTP, sa, NULL);

		//I/O redirection
		int foundIn = 0;
		int foundOut = 0;
		int inIndex, outIndex;
		//Search for "<"/">" in args
		for(int i = 0; args[i] != '\0'; i++)
		{
			if (strcmp(args[i], "<") == 0) {
				foundIn = 1;
				inIndex = i;
			}
			if (strcmp(args[i], ">") == 0) {
				foundOut = 1;
				outIndex = i;
			}
		}
		//"<" found, redirect iutput as specified, regardless of foreground or background
		if (foundIn == 1)
    {
        int fd0 = open(args[inIndex+1], O_RDONLY);
				if (fd0 == -1) {perror("source open()"); exit(1);}
        int result_in = dup2(fd0, STDIN_FILENO);
				if (result_in == -1) { perror("source dup2()"); exit(2); }
    }
		//intput not specified. If it's a background process, redirect input to null
		else
		{
			if (background_flag_signal == 1 && background_input == 1)
				void_input();
		}

		//">" found, redirect output as specified, regardless of foreground or background
		if (foundOut == 1)
		{
			int fd1 = creat(args[outIndex+1], 0644);
			if (fd1 == -1) { perror("target open()"); exit(1); }
			int result_out = dup2(fd1, STDOUT_FILENO);
			if (result_out == -1) { perror("target dup2()"); exit(2); }
		}
		//output not specified. If it's a background process, redirect output to null
		else
		{
			if (background_flag_signal == 1 && background_input == 1)
				void_output();
		}
		//Execute the child process command
		//If I/O redirected, execute the first arg ???????????????????
		if (foundIn == 1 || foundOut == 1)
			execlp(args[0], args[0], NULL);
		//I/O undirected, execute the whole command
		else
		{
			//If user input has &, execvp should ignore "&" as an arg
			//No matter foreground or background process, as long as input has &,
			//it should be ignored in child's execvp(), thus set to null terminator
			//Foreground/background difference = parent waitpid block or not
			if (background_input == 1)
				args[arg_count-1] = '\0';
			//Else, foreground process, all args are relevant
			if (execvp(args[0], args) == -1)
				perror("lsh");
		}
		//execvp never returns, except error
    exit(EXIT_FAILURE);
  }

	// Child process, error forking
	else if (pid < 0)
    perror("lsh");

	// Parent process
	else
	{
		// It does not matter if user input has & or not
		// As long as in foreground-only mode, parent should block and waitpid
		// Child process execvp does not execute & anyway
		// Foreground process, block and wait for child to complete
		if (background_flag_signal == 0 || (background_flag_signal == 1 && background_input == 0))
		{
			pid_t wpid = waitpid(pid, status, 0);
			//Foreground process terminated by ^C signal !!!
			// Process terminated by ^C is not a defunct, zombie process
			// If terminated by ^C default action, not visible from ps command
			// Therefore, the following while ... WNOHANG does not work
			// Child process terminated by ^C already has status value written, extract it
			if (WIFSIGNALED(*status))
				printf("Terminated by signal %d\n", WTERMSIG(*status));

			// Check for terminated background process, recycle zombie process
			while ((wpid = waitpid(-1, status, WNOHANG)) > 0)
			{
				printf("Child %d terminated\n", wpid);
				get_exit_status(*status);
				fflush(0);
				//Search and Reset terminated background pid to bogus value
				int j = 0;
				while(background_ids[j] != wpid)
					j++;
				background_ids[j] = -5;
			}
			return 1;
		}
		// Background process, no block, no waiting, return now
		if (background_flag_signal == 1 && background_input == 1)
		{
			printf("Background pid is %d\n", pid);
			fflush(stdout);
			// Add current running background process to an array
			int i = 0;
			while(background_ids[i] != -5)
				i++;
			background_ids[i] = pid;

			// Check for terminated background process before return
			while ((wpid = waitpid(-1, status, WNOHANG)) > 0)
			{
				printf("Child %d terminated\n", wpid);
				get_exit_status(*status);
				fflush(0);
				//Search and Reset terminated background pid to bogus value
				int j = 0;
				while(background_ids[j] != wpid)
					j++;
				background_ids[j] = -5;
			}
			return 1;
		}
  }
}

// Execute the command
int lsh_execute(char **args, int* exit_status, int background_ids[], struct sigaction* sa)
{
	// $$ expansion
	// Search for $$
	int i = 0;
	int expansion_flag = 0;
	char *ptr;
	for (i = 0; args[i] != '\0'; i++)
	{
		ptr = strstr(args[i], "$$");
		// "$$" found in current arg
		if(ptr != NULL)
		{
			// Set expansion flag
			expansion_flag = 1;
			// Replace $$ with two '\0' using pointer arithmetic
			*ptr = '\0';
			ptr++;
			*ptr = '\0';
			ptr++;
			break;
		}
	}
	// Replace $$ in the arg with pid of shell
	if (expansion_flag == 1)
	{
		// Get pid and convert pid_t from number to string
		char buffer[256];
		char pid_str[50];
		memset(pid_str, '\0', 50);
		memset(buffer, '\0', 256);
		int expansion_pid = getpid();
		sprintf(pid_str, "%d", expansion_pid);
		// printf("pid = %d\n", expansion_pid);
		// Copy string before $$, expanded pid, and string after $$ into buffer
		strcpy(buffer, args[i]);
		strcat(buffer, pid_str);
		strcat(buffer, ptr);
		// Replace input with expanded argument
		args[i] = buffer;
	}

	//Empty line
	if (args[0] == NULL)
		return 1;
	//Comment line
	if (startsWith(args[0], "#") == 1)
		return 1;
	//Builtin commands
	// cd builtin
	if (startsWith(args[0], "cd") == 1)
	{
		lsh_cd(args, exit_status);
		return 1;
	}
	// exit builtin
	if (startsWith(args[0], "exit") == 1)
	{
		lsh_exit(args, exit_status, background_ids);
		return 0;
	}
	// status builtin
	if (startsWith(args[0], "status") == 1)
	{
		lsh_status(args, exit_status);
		return 1;
	}
	//Execute commands other than builtins
  return lsh_launch(args, exit_status, background_ids, sa);
}

// Keep taking user input until exit is executed
void lsh_loop(void)
{
  char *line;
  char **args;
  int _continue;
	int exit_status = 0;
	// Array to store background child processes
	int background_children[1000] = {-5};
	// Init all pid to bogus value
	for (int i = 0; i < 1000; i++)
		background_children[i] = -5;

	// Handle SIGTSTP
	struct sigaction SIGTSTP_action = {0};
	SIGTSTP_action.sa_handler = catchSIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART;
	//Register signals with sigaction struct
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);

	// Take user input, parse and execute each command
  do {
    printf(": ");
    line = lsh_read_line();
    args = lsh_split_line(line);
		// If input is not exit, _continue = 1
    _continue = lsh_execute(args, &exit_status, background_children, &SIGTSTP_action);
		// printf("args 1 = %s\n", args[0]);
		// printf("args 2 = %s\n", args[1]);
    free(line);
    free(args);
  } while (_continue);
}

int main(int argc, char **argv)
{
  // Run command loop.
  lsh_loop();
  return EXIT_SUCCESS;
}
