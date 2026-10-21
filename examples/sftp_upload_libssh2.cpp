// Example: upload a file through SFTP using libssh2.
//
// This is the kind of implementation usually meant by "experience with SFTP/SSH":
// your application does not implement SSH itself; it uses a battle-tested library
// and is responsible for configuration, authentication, error handling, and cleanup.
//
// macOS install example:
//   brew install libssh2
//
// Build example, depending on your include/library paths:
//   clang++ -std=c++17 examples/sftp_upload_libssh2.cpp -lssh2 -o build/sftp_upload_libssh2
//
// Usage:
//   ./build/sftp_upload_libssh2 host user ~/.ssh/id_ed25519 ./local.txt /upload/local.txt

#include <arpa/inet.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() {
        if (value_ >= 0) {
            close(value_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : value_(other.value_) {
        other.value_ = -1;
    }

    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                close(value_);
            }
            value_ = other.value_;
            other.value_ = -1;
        }
        return *this;
    }

    int get() const { return value_; }

private:
    int value_;
};

std::string last_session_error(LIBSSH2_SESSION* session) {
    char* message = nullptr;
    int length = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    return message == nullptr ? "unknown libssh2 error" : std::string(message, length);
}

Fd connect_tcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        Fd socket_fd(socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (socket_fd.get() < 0) {
            continue;
        }

        if (connect(socket_fd.get(), current->ai_addr, current->ai_addrlen) == 0) {
            freeaddrinfo(results);
            return socket_fd;
        }
    }

    freeaddrinfo(results);
    throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
}

void print_host_key_fingerprint(LIBSSH2_SESSION* session) {
    // Real systems should verify the host key against a trusted known_hosts file.
    // Printing it is useful for this example because it shows where that decision
    // belongs in the SSH flow: after handshake, before trusting the server.
    const unsigned char* hash = reinterpret_cast<const unsigned char*>(
        libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1));
    if (hash == nullptr) {
        throw std::runtime_error("could not read host key fingerprint");
    }

    std::cout << "server SHA1 fingerprint:";
    for (int i = 0; i < 20; ++i) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
}

void authenticate_with_private_key(
    LIBSSH2_SESSION* session,
    const std::string& username,
    const std::string& private_key_path) {
    // Passing nullptr as the public key lets libssh2 derive/use the public side
    // when possible. For encrypted keys, replace the final nullptr with a passphrase.
    const int rc = libssh2_userauth_publickey_fromfile(
        session,
        username.c_str(),
        nullptr,
        private_key_path.c_str(),
        nullptr);

    if (rc != 0) {
        throw std::runtime_error("SSH public-key auth failed: " + last_session_error(session));
    }
}

void upload_file(LIBSSH2_SFTP* sftp, const std::string& local_path, const std::string& remote_path) {
    std::ifstream input(local_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open local file: " + local_path);
    }

    LIBSSH2_SFTP_HANDLE* remote_file = libssh2_sftp_open(
        sftp,
        remote_path.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        0644);
    if (remote_file == nullptr) {
        throw std::runtime_error("could not open remote file: " + remote_path);
    }

    std::array<char, 16 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize bytes_read = input.gcount();
        char* cursor = buffer.data();

        // SFTP writes can be partial, just like TCP writes. Keep writing until
        // the chunk is fully sent or libssh2 reports an error.
        while (bytes_read > 0) {
            const ssize_t bytes_written = libssh2_sftp_write(remote_file, cursor, bytes_read);
            if (bytes_written < 0) {
                libssh2_sftp_close(remote_file);
                throw std::runtime_error("SFTP write failed");
            }
            cursor += bytes_written;
            bytes_read -= bytes_written;
        }
    }

    libssh2_sftp_close(remote_file);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 6 && argc != 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <host> <user> <private-key> <local-file> <remote-file> [port]\n";
        return 2;
    }

    const std::string host = argv[1];
    const std::string username = argv[2];
    const std::string private_key_path = argv[3];
    const std::string local_path = argv[4];
    const std::string remote_path = argv[5];

    try {
        const std::string port = argc == 7 ? argv[6] : "22";
        if (port.empty() || port.size() > 5 ||
            port.find_first_not_of("0123456789") != std::string::npos ||
            std::stoi(port) < 1 || std::stoi(port) > 65535) {
            throw std::runtime_error("invalid port");
        }
        if (libssh2_init(0) != 0) {
            throw std::runtime_error("libssh2_init failed");
        }

        Fd socket_fd = connect_tcp(host, port);

        LIBSSH2_SESSION* session = libssh2_session_init();
        if (session == nullptr) {
            throw std::runtime_error("libssh2_session_init failed");
        }

        // Handshake upgrades the already-connected TCP socket into an SSH session.
        if (libssh2_session_handshake(session, socket_fd.get()) != 0) {
            throw std::runtime_error("SSH handshake failed: " + last_session_error(session));
        }

        print_host_key_fingerprint(session);
        authenticate_with_private_key(session, username, private_key_path);

        // SFTP is opened as an SSH subsystem. It is not FTP; it rides inside SSH.
        LIBSSH2_SFTP* sftp = libssh2_sftp_init(session);
        if (sftp == nullptr) {
            throw std::runtime_error("could not start SFTP subsystem: " + last_session_error(session));
        }

        upload_file(sftp, local_path, remote_path);
        std::cout << "uploaded " << local_path << " to " << host << ':' << remote_path << '\n';

        libssh2_sftp_shutdown(sftp);
        libssh2_session_disconnect(session, "normal shutdown");
        libssh2_session_free(session);
        libssh2_exit();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        libssh2_exit();
        return 1;
    }

    return 0;
}
