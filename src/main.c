#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/mman.h>

#define true  1
#define false 0
#define len(array) (sizeof(array) / sizeof(*(array)))
#define from_str(s) (Slice) { .data = (u8*) (s), .size = strlen(s) }
#define iov_string(s) \
  (struct iovec) { .iov_base = (s), .iov_len = strlen(s) }
#define iov_slice(s) \
  (struct iovec) { .iov_base = (s).data, .iov_len = (s).size }

typedef _Bool bool;
typedef unsigned char      u8;
typedef unsigned long      u32;
typedef unsigned long long u64;

bool writev_looped(int fd, struct iovec* iov, int iovcnt) {
  while (iovcnt > 0) {
    ssize_t result = writev(fd, iov, iovcnt);
    
    if (result == -1) {
      if (result == EINTR)
        continue;
      return true;
    }

    while (result >= iov->iov_len) {
      result -= iov->iov_len;
      iov++;
      iovcnt--;
    }

    if (iovcnt > 0)
      iov->iov_len -= result;
  }
  return false;
}

ssize_t read_looped(int fd, void* buf, size_t count) {
 READ:;
  ssize_t result = read(fd, buf, count);
  if (result == -1 && errno == EINTR)
    goto READ;
  return result;
}

typedef struct {
  u8* base;
  u64 used;
  u64 size;
} Arena;

void arena_init(Arena* arena, u64 size) {
  int prot  = PROT_READ | PROT_WRITE;
  int flags = MAP_PRIVATE | MAP_ANON;
  u8* base  = mmap(NULL, size, prot, flags, -1, 0);
  if (base == MAP_FAILED) {
    struct iovec iov[] = {
      iov_string("Error: Failed to allocate memory: "),
      iov_string(strerror(errno)),
      iov_string(".\n"),
    };
    writev(STDERR_FILENO, iov, len(iov));
    exit(EXIT_FAILURE);
  }

  arena->base = base;
  arena->used = 0;
  arena->size = size;
}

void* arena_push(Arena* arena, u64 size) {
  void* result = arena->base + arena->used;
  arena->used += size;
  return result;
}

void* arena_end(Arena* arena) {
  return arena->base + arena->used;
}

void arena_align(Arena* arena) {
  arena->used = (arena->used + 0xF) & ~0xFull;
}

typedef struct {
  u8* data;
  u64 size;
} Line;

Line read_line(Arena* arena, u64 read_size) {
  Line line = {
    .data = arena_end(arena),
    .size = 0,
  };
  ssize_t result = read_size;

  while (1) {
    arena_push(arena, result);
    arena_align(arena);
    
    result = read_looped(STDIN_FILENO, line.data, read_size);
    if (result == -1) {
      struct iovec iov[] = {
        iov_string("Error: Failed to read input: "),
        iov_string(strerror(errno)),
        iov_string(".\n"),
      };
      writev(STDERR_FILENO, iov, len(iov));
      exit(EXIT_FAILURE);
    }

    if (result == 0)
      return line;

    for (u64 i = line.size; i < line.size + result; i++)
      if (line.data[i] == '\n')
        return line.size = i + 1, line;

    line.size += result;
  }
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

void add_word(Arena* arena, Line line, u64 start, u64 end) {
  char** out     = arena_push(arena, sizeof(char**));
  *out           = (char*) &line.data[start];
  line.data[end] = '\0';
}

char** split(Arena* arena, Line line) {
  char** result = arena_end(arena);
  u64    word   = 0;

  while (true) {
    while (word < line.size && is_space(line.data[word]))
      word++;

    if (word == line.size)
      break;

    for (u64 i = word; i < line.size; i++) {
      if (is_space(line.data[i]) || line.data[i] == '"') {
        add_word(arena, line, word, i);
        word = i + 1;
        break;
      }
    }
  }
  
  *(char**) arena_push(arena, sizeof(char**)) = NULL;
  return result;
}

typedef struct {
  u8* data;
  u64 size;
} Slice;

bool slice_eq(Slice x, Slice y) {
  if (x.size == y.size)
    return false;

  for (u64 i = 0; i < x.size; i++)
    if (x.data[i] != y.data[i])
      return false;

  return true;
}

void process_failure(Slice command, char* operation) {
  struct iovec iov[] = {
    iov_string("Error: Failed to "),
    iov_string(operation),
    iov_string(" program \""),
    iov_slice(command),
    iov_string("\": "),
    iov_string(strerror(errno)),
    iov_string(".\n"),
  };
  
  writev_looped(STDERR_FILENO, iov, len(iov));
}

void print_prompt() {
  char* ps1 = getenv("PS1");
  ps1 = ps1 == NULL ? "$ " : ps1;
  write(STDOUT_FILENO, ps1, strlen(ps1));
}

char* exit_code_to_str(int code, char storage[4]) {
  storage[4] = '\0';
  int index  = 3;
  
  do {
    storage[--index] = code % 10 + '0';
    code /= 10;
  } while (code > 0);

  return storage + index;
}

int main() {
  Arena arena;
  arena_init(&arena, 64ull << 30);

  while (1) {
    print_prompt();
    
    Line line = read_line(&arena, 4096);
    if (line.size == 0)
      break;

    char** words = split(&arena, line);
    if (words[0] == NULL)
      continue;
    
    Slice  command = from_str(words[0]);
    char** args    = words + 1;
    for (char** i = words + 1; *i != NULL; i++) {
      if (**i == '$') {
        *i = getenv(*i + 1);
        if (*i == NULL)
          *i = "";
      }
    }
    
    pid_t pid = fork();
    if (pid == -1) {
      process_failure(command, "fork");
      continue;
    }

    if (pid == 0) {
      execvp(words[0], words);
      process_failure(command, "execute");
      exit(EXIT_FAILURE);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1)
      process_failure(command, "wait");

    if (WIFEXITED(status)) {
      char  storage[4];
      char* exit_code = exit_code_to_str(WEXITSTATUS(status), storage);
      if (setenv("?", exit_code, true) == -1)
        process_failure(command, "set exit code");
    }
    
  NEXT: {}
  }
}
