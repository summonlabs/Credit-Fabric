// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "support/process.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
// windows.h must not drag in winsock.h ahead of anything else here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cftest {

namespace {

#if defined(_WIN32)

std::string quote(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') out += "\\";
    out += c;
  }
  out += "\"";
  return out;
}

#else

std::string quote(const std::string& value) { return "'" + value + "'"; }

#endif

}  // namespace

ChildProcess::~ChildProcess() {
  if (process_ != nullptr) {
    (void)kill();
    (void)wait();
  }
  if (output_read_ != nullptr) {
#if defined(_WIN32)
    ::CloseHandle(static_cast<HANDLE>(output_read_));
#else
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(output_read_)));
#endif
    output_read_ = nullptr;
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(other.process_),
      output_read_(other.output_read_),
      pid_(other.pid_),
      exited_(other.exited_),
      exit_code_(other.exit_code_),
      output_closed_(other.output_closed_),
      buffer_(std::move(other.buffer_)) {
  other.process_ = nullptr;
  other.output_read_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    process_ = other.process_;
    output_read_ = other.output_read_;
    pid_ = other.pid_;
    exited_ = other.exited_;
    exit_code_ = other.exit_code_;
    output_closed_ = other.output_closed_;
    buffer_ = std::move(other.buffer_);
    other.process_ = nullptr;
    other.output_read_ = nullptr;
  }
  return *this;
}

#if defined(_WIN32)

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments, ChildProcess& out) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) return false;
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  std::string command = quote(executable);
  for (const std::string& argument : arguments) {
    command += " ";
    command += quote(argument);
  }
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &information);
  ::CloseHandle(write_end);
  if (created == 0) {
    ::CloseHandle(read_end);
    return false;
  }

  out.process_ = information.hProcess;
  out.output_read_ = read_end;
  out.pid_ = static_cast<int>(information.dwProcessId);
  ::CloseHandle(information.hThread);
  return true;
}

bool ChildProcess::running() {
  if (process_ == nullptr || exited_) return false;
  const DWORD status = ::WaitForSingleObject(static_cast<HANDLE>(process_), 0);
  if (status == WAIT_TIMEOUT) return true;
  exited_ = true;
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) != 0) exit_code_ = static_cast<int>(code);
  return false;
}

bool ChildProcess::kill() {
  if (process_ == nullptr) return false;
  const BOOL killed = ::TerminateProcess(static_cast<HANDLE>(process_), 137);
  exited_ = false;
  return killed != 0;
}

int ChildProcess::wait() {
  if (process_ == nullptr) return -1;
  if (!exited_) {
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    (void)::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
  }
  return exit_code_;
}

std::string ChildProcess::read_line() {
  if (output_read_ == nullptr || output_closed_) return std::string();
  auto* handle = static_cast<HANDLE>(output_read_);
  for (;;) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    char chunk[512];
    DWORD read = 0;
    if (::ReadFile(handle, chunk, sizeof(chunk), &read, nullptr) == 0 || read == 0) {
      output_closed_ = true;
      std::string line = buffer_;
      buffer_.clear();
      return line;
    }
    buffer_.append(chunk, read);
  }
}

std::string ChildProcess::drain() {
  std::string out;
  while (!output_closed_) {
    const std::string line = read_line();
    if (line.empty() && output_closed_) break;
    out += line;
    out += "\n";
    if (output_closed_) break;
  }
  return out;
}

#else  // POSIX

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments, ChildProcess& out) {
  int descriptors[2];
  if (::pipe(descriptors) != 0) return false;
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return false;
  }
  if (pid == 0) {
    ::close(descriptors[0]);
    ::dup2(descriptors[1], STDOUT_FILENO);
    ::dup2(descriptors[1], STDERR_FILENO);
    ::close(descriptors[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(descriptors[1]);
  out.process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  out.output_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptors[0]));
  out.pid_ = static_cast<int>(pid);
  return true;
}

bool ChildProcess::running() {
  if (process_ == nullptr || exited_) return false;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  int status = 0;
  const pid_t result = ::waitpid(pid, &status, WNOHANG);
  if (result == 0) return true;
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return false;
}

bool ChildProcess::kill() {
  if (process_ == nullptr) return false;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
  return ::kill(pid, SIGKILL) == 0;
}

int ChildProcess::wait() {
  if (process_ == nullptr) return -1;
  if (!exited_) {
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  return exit_code_;
}

std::string ChildProcess::read_line() {
  if (output_read_ == nullptr || output_closed_) return std::string();
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(output_read_));
  for (;;) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    char chunk[512];
    const ssize_t read = ::read(descriptor, chunk, sizeof(chunk));
    if (read <= 0) {
      output_closed_ = true;
      std::string line = buffer_;
      buffer_.clear();
      return line;
    }
    buffer_.append(chunk, static_cast<std::size_t>(read));
  }
}

std::string ChildProcess::drain() {
  std::string out;
  while (!output_closed_) {
    const std::string line = read_line();
    if (line.empty() && output_closed_) break;
    out += line;
    out += "\n";
  }
  return out;
}

#endif

}  // namespace cftest
