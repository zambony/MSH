#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>

/**
 * MSH!
 * Currently supports:
 * 	• File input
 * 	• Input directed from bash '<'
 * 	• Multi-command queue (semicolon)
 * 	• Quoted arguments
 * 	• Redirects!
 */

// Comment out to make the program prettier
#define SUBMISSION

#define COLOR_RESET         "\033[0m"
#define COLOR_C             "\033[1;38;5;129m"

// What color is the MSH prompt prefix?
#define COLOR_MSH COLOR_C

#ifdef SUBMISSION
#define ERROR(cmd) fprintf(stdout, "msh: %s: %s\n", cmd, strerror(errno))
#else
#define ERROR(cmd) fprintf(stderr, "msh: %s: %s\n", cmd, strerror(errno))
#endif

#define BEGIN_REDIRECT(inFile, outFile) \
int __STDOUT_SAVE = -1;\
int __STDIN_SAVE = -1;\
if (outFile && strcmp(outFile, ""))\
{\
    int fdOut = open(outFile, O_WRONLY | O_CREAT | O_TRUNC, 0600);\
    if (fdOut != -1)\
    {\
        __STDOUT_SAVE = dup(STDOUT_FILENO);\
        dup2(fdOut, STDOUT_FILENO);\
        close(fdOut);\
    }\
}\
if (inFile && strcmp(inFile, ""))\
{\
	int fdIn = open(inFile, O_RDONLY);\
	if (fdIn != -1)\
	{\
		__STDIN_SAVE = dup(STDIN_FILENO);\
		dup2(fdIn, STDIN_FILENO);\
		close(fdIn);\
	}\
}\

#define END_REDIRECT() \
if (__STDOUT_SAVE != -1)\
{\
	dup2(__STDOUT_SAVE, STDOUT_FILENO);\
	close(__STDOUT_SAVE);\
}\
if (__STDIN_SAVE != -1)\
{\
	dup2(__STDIN_SAVE, STDIN_FILENO);\
	close(__STDIN_SAVE);\
}\
fflush(stdout);\
fflush(stdin);\

#define RUN_DIRECTED(inFile, outFile, func) \
({\
	BEGIN_REDIRECT(inFile, outFile)\
	func\
	END_REDIRECT()\
})\

typedef enum BOOL
{
	false = 0,
	true  = 1
} bool;

// Struct to hold function name and function
typedef struct func_pair
{
	char *name;

	void (*func)(char **args);
} func_pair_t;

// Forward declarations for builtins
void builtin_help(char **args);

void builtin_today(char **args);

void builtin_cd(char **args);

// Lookup table for each builtin command
func_pair_t func_table[] = {
	{"cd",    builtin_cd},
	{"today", builtin_today},
	{"help",  builtin_help},
};

// Retrieve a function pointer given its name.
void (*func_lookup(const char *name))()
{
	if (!name)
	{
		return NULL;
	}

	unsigned int i;

	/// TODO: maybe make it binary search?
	for (i = 0; i < sizeof(func_table) / sizeof(func_table[0]); i++)
	{
		if (!strcmp(name, func_table[i].name))
		{
			return func_table[i].func;
		}
	}

	return NULL;
}

// Global command line arguments
char **_argv;
bool bFile = false;

char *trim(char *str)
{
	char *end;

	// Trim leading space
	while (isspace((unsigned char) *str)) str++;

	if (*str == 0)
	{  // All spaces?
		return str;
	}

	// Trim trailing space
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char) *end)) end--;

	// Write new null terminator character
	end[1] = '\0';

	return str;
}

#define CHUNK_SIZE 256
#define MAX_CHUNKS 4

/**
 * Homemade readline. Dynamically read input.
 *
 * @param prompt The text to prefix the user's input with
 * @param line Assigned the value of the user's input if not NULL
 *
 * @return The size of the read input
 */
ssize_t readline(const char *prompt, char **line)
{
	if (prompt && isatty(STDIN_FILENO))
	{
		fputs(prompt, stdout);
	}

	*line = (char *) malloc(CHUNK_SIZE);
	size_t bufSize = CHUNK_SIZE;

	char    read;
	ssize_t pos = 0;
	while ((read = getchar()) != '\n' && bufSize < (CHUNK_SIZE * MAX_CHUNKS))
	{
		if ((int) read == EOF)
		{
			errno = 0;

			return -1;
		}

		(*line)[pos++] = read;

		if (pos == bufSize)
		{
			bufSize += CHUNK_SIZE;  // grow our buffer with extra padding in case
			*line = realloc(*line, bufSize);
		}
	}

	if (pos > 0)
	{
		// Shrink, but make room for \0
		*line = realloc(*line, (pos + 1));

		// dont forget to TERMINATE
		(*line)[pos] = '\0';
	}

	return pos;
}

/**
 * Tokenize a string and return an array of strings.
 *
 * @param string The string to break apart
 * @param delim The character to use for separation
 * @param numTokens Number of entries is assigned to this variable if not NULL
 *
 * @return The token array
 */
char **explode(const char *string, const char* delim, size_t *const numTokens)
{
	char   *duplicate      = strdup(string);  // duplicate original string because strtok_r is destructive
	size_t tokensAllocated = 1;
	size_t tokenCount      = 0;
	char   *save;
	char   **tokenList     = calloc(tokensAllocated, sizeof(char *));  // allocate an array to store whole words per-index
	char   *current        = strtok_r(duplicate, delim, &save);  // enter the tokenizing sequence

	while (current != NULL)
	{
		if (tokenCount == tokensAllocated)
		{
			tokensAllocated *= 2;  // grow buffer more than needed for safety
			tokenList = realloc(tokenList, tokensAllocated * sizeof(char *));
		}

		// make a copy of the token and store it.
		// we do this because we free 'duplicate' later,
		// and if 'duplicate' is gone, strtok_r's pointers will lead nowhere
		tokenList[tokenCount++] = strdup(trim(current));

		current = strtok_r(NULL, delim, &save);  // move to next token
	}

	if (tokenCount == 0)  // if we have no tokens just delete our list
	{
		free(tokenList);
		tokenList = NULL;
	}
	else  // otherwise shrink to fit!
	{
		tokenList = realloc(tokenList, tokenCount * sizeof(char *));
	}

	*numTokens = tokenCount;

	free(duplicate);  // cleanup our duplicated string

	return tokenList;
}

/**
 * Tokenize a string using a custom separator and obey tags (such as chevrons or double quotes)
 *
 * When finished, remember to free each entry and then the array itself
 *
 * @param string The string to break apart
 * @param separator The separator to break tokens apart by
 * @param openTag The character which signals the start of a tagged argument
 * @param closeTag The character which signals the end of a tagged argument
 * @param bRemoveTag Whether or not to remove the tags from the final result
 * @param numTokens Number of entries is assigned to this variable if not NULL
 *
 * @return The token array
 */
char **explodeByTag(const char *string,
                    const char separator,
                    const char openTag,
                    const char closeTag,
                    bool bRemoveTag,
                    size_t *const numTokens)
{
	size_t tokensAllocated = 1;
	size_t tokenCount      = 0;

	char **tokenList = calloc(tokensAllocated, sizeof(char *));  // allocate an array to store whole words per-index

	char    *buffer = (char *) malloc(CHUNK_SIZE);
	ssize_t bufSize = CHUNK_SIZE;

	bool bTag = false;
	int  i    = 0;  // which character are we reading from the string
	int  pos  = 0;  // current length of the read buffer, used to track where to append new characters

	for (i = 0; string[i] != 0; i++)
	{
		char read = string[i];

		if (tokenCount == tokensAllocated)
		{
			tokensAllocated *= 2;  // grow buffer more than needed for safety
			tokenList = realloc(tokenList, tokensAllocated * sizeof(char *));
		}

		// If we're not inside a tag...
		if (!bTag)
		{
			// If the current character is the opening tag...
			if (read == openTag)
			{
				// If they would like to keep the tag...
				if (!bRemoveTag)
				{
					// Concatenate the current character
					buffer[pos++] = read;
				}

				// Set state to inside tag
				bTag = true;
			}
				// We encountered a regular separator, concatenate the current buffer
			else if (read == separator)
			{
				// Terminate string, and reset buffer size
				buffer[pos] = '\0';

				tokenList[tokenCount++] = strdup(trim(buffer));

				// Clear the contents of the buffer. After clear, reset buffer size and set the buffer to the default size
				memset(buffer, 0, bufSize + 1);
				bufSize = CHUNK_SIZE;
				buffer  = realloc(buffer, bufSize);

				pos = 0;
			}
			else
			{
				buffer[pos++] = read;
			}
		}
		else
		{
			if (read == closeTag)
			{
				if (!bRemoveTag)
				{
					buffer[pos++] = read;
				}

				bTag = false;
			}
			else
			{
				buffer[pos++] = read;
			}
		}

		// The length of the current buffer has exceeded the buffer size, reallocate!
		if (pos == bufSize)
		{
			bufSize += CHUNK_SIZE;
			buffer = realloc(buffer, bufSize);
		}
	}

	if (buffer[0] != 0)
	{
		tokenList = realloc(tokenList, (tokenCount + 1) * sizeof(char *));

		buffer = realloc(buffer, pos + 1);
		buffer[pos] = '\0';
		tokenList[tokenCount++] = strdup(trim(buffer));
	}

	if (tokenCount == 0)  // if we have no tokens just delete our list
	{
		free(tokenList);
		tokenList = NULL;
	}
	else  // otherwise shrink to fit!
	{
		tokenList = realloc(tokenList, tokenCount * sizeof(char *));
	}

	*numTokens = tokenCount;

	free(buffer);  // clean the buffer out

	return tokenList;
}

/**
 * Run a named command.
 *
 * @param cmd Case-sensitive command name
 * @param length The length of the command string
 */
void runCmd(char *cmd, ssize_t length)
{
	if (!strcmp("exit", cmd))  // i typed exit, QUIT!!!
	{
#ifndef SUBMISSION
		puts("exit");
#endif
		free(cmd);

		exit(0);
	}

	size_t tokenCount = 0;

	char **args = explodeByTag(cmd, ' ', '"', '"', true, &tokenCount);  // cleanup: args, args[N]
	args = realloc(args, (tokenCount + 1) * sizeof(char *));
	args[tokenCount] = NULL;

	// Ensure we actually parsed some args
	if (tokenCount > 0)
	{
		void (*pBuiltin)(char **) = func_lookup(args[0]);

		char *outFile = NULL;
		char *inFile = NULL;

		// Iterate over tokens and check for redirects
		unsigned int tk;
		for (tk = 0; tk < tokenCount; tk++)
		{
			char *token = args[tk];

			if (!strcmp("<", token))
			{
				inFile = strdup(args[tk + 1]);
				args[tk] = NULL;
				args[tk + 1] = NULL;
				tk++;
			}
			else if (!strcmp(">", token))
			{
				outFile = strdup(args[tk + 1]);
				args[tk] = NULL;
				args[tk + 1] = NULL;
				tk++;
			}
		}

		// Run the builtin in the main process, as it may change important
		// info in the shell state. Only children need a fork, as they execute commands
		if (pBuiltin != NULL)
		{
			pBuiltin(args);
		}
		else
		{
			// Give birth to a child and work them to death
			pid_t id = fork();  // cleanup: PROCESS

			if (id == 0)  // if we're the child, do the parsing
			{
				// smol code, i love it
				RUN_DIRECTED(inFile, outFile, { // NOLINT(bugprone-suspicious-string-compare,hicpp-signed-bitwise)
					int status = execvp(args[0], args);  // run that bad boy!!!

					// Error up here so it's redirected to the file...
					if (status == -1)
					{
						ERROR(args[0]);
					}
				});

				// Kill child, no zombies allowed
				exit(0);
			}
			else  // wait for child to finish
			{
				wait(NULL);
			}
		}

		free(outFile);
		free(inFile);
	}

	// Cleanup the token list
	size_t i;

	for (i = 0; i < tokenCount; i++)
	{
		free(args[i]);
	}

	free(args);
}

/**
 * Attempt to run a file as input, rather than prompting STDIN.
 *
 * @param fp The file stream to read
 */
void runfile(FILE *fp)
{
	char   *pOut   = (char *) malloc(CHUNK_SIZE);
	size_t bufSize = CHUNK_SIZE;

	char    read;
	ssize_t pos = 0;

	while ((read = fgetc(fp)) != EOF)
	{
		if (read == '\n')
		{
			// Shrink, but make room for \0
			pOut = realloc(pOut, (pos + 1));

			// dont forget to TERMINATE
			pOut[pos] = '\0';

			size_t numCommands;
			char   **commandQueue = explode(pOut, ";", &numCommands);

			if (commandQueue && numCommands > 0)
			{
				int i;
				for (i = 0; i < numCommands; i++)
				{
					runCmd(commandQueue[i], pos);
					free(commandQueue[i]);
				}
			}

			free(commandQueue);

			// Clear the contents of the buffer
			memset(pOut, 0, (pos + 1));
			pos = 0;

			// Skip to the next line, don't read anything else into buffer
			continue;
		}

		pOut[pos++] = read;

		if (pos == bufSize)
		{
			bufSize += CHUNK_SIZE;  // grow our buffer with extra padding in case
			pOut = realloc(pOut, bufSize);
		}
	}

	free(pOut);
}

/**
 * Simple call to request input from the user and
 * attempt to execute a command from it.
 */
void processInput()
{
	if (bFile)
	{
		FILE *fp = fopen(_argv[1], "r");

		// Failed to find the file
		if (!fp)
		{
			ERROR(_argv[1]);

			exit(0);
		}

		runfile(fp);
		fclose(fp);
		bFile = 0;

		exit(0);
	}
	else
	{
		char *text = NULL;
#ifdef SUBMISSION
		const char *prefix = "msh> ";
#else
		const char *prefix = COLOR_MSH"msh"COLOR_RESET"> ";
#endif

		ssize_t length = readline(prefix, &text);

		if (length == 0)
		{
			free(text);

			return;
		}
		else if (length == -1 && errno == 0)
		{
			if (isatty(STDIN_FILENO))
			{
#ifdef SUBMISSION
				puts("");
#else
				puts("exit");
#endif
			}

			free(text);

			exit(0);
		}

		size_t numCommands;
		char **commandQueue = explode(text, ";", &numCommands);

		if (commandQueue && numCommands > 0)
		{
			int i;
			for (i = 0; i < numCommands; i++)
			{
				runCmd(commandQueue[i], length);
				free(commandQueue[i]);
			}
		}

		free(commandQueue);

//		runCmd(text, length);
		free(text);
	}
}

/*
 * Builtin Declarations
 */

/**
 * Prints the help message for MSH
 *
 * @param args Array of arguments supplied for the command
 */
void builtin_help(char **args)
{
	puts("enter Linux commands, or 'exit' to exit");
}

/**
 * Prints the current date of the server
 *
 * @param args Array of arguments supplied for the command
 */
void builtin_today(char **args)
{
	time_t    rawtime;
	struct tm *timeinfo;

	time(&rawtime);
	timeinfo = localtime(&rawtime);

	printf("%02d/%02d/%04d\n", timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900);
}

/**
 * Set current working directory
 *
 * @param args Array of arguments supplied for the command
 */
void builtin_cd(char **args)
{
	const char *dir = args[1] ?: getenv("HOME");

	int status = chdir(dir);

	if (status == -1)
	{
		char *message = (char *) malloc(4 + strlen(dir) + 1);  // length of 'cd: ' + length of directory name + terminator
		strcpy(message, "cd: ");
		strcat(message, dir);

		ERROR(message);

		free(message);
	}
}

/*
 * End Builtin Declarations
 */

int main(int argc, char **argv)
{
	_argv = argv;

	if (_argv[1])
	{
		bFile = true;
	}

	// No need for exit condition, functions exit by themselves
	while (1)
	{
		processInput();
	}

	return 0;
}
