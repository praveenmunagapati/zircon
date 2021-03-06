// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/auto_call.h>
#include <fcntl.h>
#include <fdio/remoteio.h>
#include <fdio/watcher.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __Fuchsia__
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <fs/connection.h>
#include <fs/remote.h>
#include <threads.h>
#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zx/event.h>
#endif

#include <fs/trace.h>
#include <fs/vfs.h>
#include <fs/vnode.h>

uint32_t __trace_bits;

namespace fs {
namespace {

bool is_dot(const char* name, size_t len) {
    return len == 1 && strncmp(name, ".", len) == 0;
}

bool is_dot_dot(const char* name, size_t len) {
    return len == 2 && strncmp(name, "..", len) == 0;
}

#ifdef __Fuchsia__ // Only to prevent "unused function" warning
bool is_dot_or_dot_dot(const char* name, size_t len) {
    return is_dot(name, len) || is_dot_dot(name, len);
}
#endif

// Trim a name before sending it to internal filesystem functions.
// Trailing '/' characters imply that the name must refer to a directory.
zx_status_t vfs_name_trim(const char* name, size_t len, size_t* len_out, bool* dir_out) {
    bool is_dir = false;
    while ((len > 0) && name[len - 1] == '/') {
        len--;
        is_dir = true;
    }

    // 'name' should not contain paths consisting of exclusively '/' characters.
    if (len == 0) {
        return ZX_ERR_INVALID_ARGS;
    } else if (len > NAME_MAX) {
        return ZX_ERR_BAD_PATH;
    }

    *len_out = len;
    *dir_out = is_dir;
    return ZX_OK;
}

zx_status_t vfs_lookup(fbl::RefPtr<Vnode> vn, fbl::RefPtr<Vnode>* out,
                       const char* name, size_t len) {
    if (is_dot_dot(name, len)) {
        return ZX_ERR_INVALID_ARGS;
    } else if (is_dot(name, len)) {
        *out = fbl::move(vn);
        return ZX_OK;
    }
    return vn->Lookup(out, name, len);
}

// Validate open flags as much as they can be validated
// independently of the target node.
zx_status_t vfs_validate_flags(uint32_t flags) {
    switch (flags & O_ACCMODE) {
    case O_RDONLY:
        if (flags & O_TRUNC) {
            return ZX_ERR_INVALID_ARGS;
        }
    case O_WRONLY:
    case O_RDWR:
        return ZX_OK;
    default:
        return ZX_ERR_INVALID_ARGS;
    }
}

} // namespace

#ifdef __Fuchsia__

bool RemoteContainer::IsRemote() const {
    return remote_.is_valid();
}

zx::channel RemoteContainer::DetachRemote(uint32_t& flags_) {
    flags_ &= ~VFS_FLAG_MOUNT_READY;
    return fbl::move(remote_);
}

// Access the remote handle if it's ready -- otherwise, return an error.
zx_handle_t RemoteContainer::WaitForRemote(uint32_t& flags_) {
    if (!remote_.is_valid()) {
        // Trying to get remote on a non-remote vnode
        return ZX_ERR_UNAVAILABLE;
    } else if (!(flags_ & VFS_FLAG_MOUNT_READY)) {
        zx_signals_t observed;
        zx_status_t status = remote_.wait_one(ZX_USER_SIGNAL_0 | ZX_CHANNEL_PEER_CLOSED,
                                              0,
                                              &observed);
        // Not set (or otherwise remote is bad)
        // TODO(planders): Add a background thread that waits on all remotes
        if (observed & ZX_CHANNEL_PEER_CLOSED) {
            return ZX_ERR_PEER_CLOSED;
        } else if ((status != ZX_OK)) {
            return ZX_ERR_UNAVAILABLE;
        }

        flags_ |= VFS_FLAG_MOUNT_READY;
    }
    return remote_.get();
}

zx_handle_t RemoteContainer::GetRemote() const {
    return remote_.get();
}

void RemoteContainer::SetRemote(zx::channel remote) {
    ZX_DEBUG_ASSERT(!remote_.is_valid());
    remote_ = fbl::move(remote);
}

#endif

Vfs::Vfs() = default;
Vfs::~Vfs() = default;

#ifdef __Fuchsia__
Vfs::Vfs(async_t* async)
    : async_(async) {}
#endif

zx_status_t Vfs::Open(fbl::RefPtr<Vnode> vndir, fbl::RefPtr<Vnode>* out,
                      const char* path, const char** pathout, uint32_t flags,
                      uint32_t mode) {
#ifdef __Fuchsia__
    fbl::AutoLock lock(&vfs_lock_);
#endif
    return OpenLocked(fbl::move(vndir), out, path, pathout, flags, mode);
}

zx_status_t Vfs::OpenLocked(fbl::RefPtr<Vnode> vndir, fbl::RefPtr<Vnode>* out,
                            const char* path, const char** pathout, uint32_t flags,
                            uint32_t mode) {
    FS_TRACE(VFS, "VfsOpen: path='%s' flags=%d\n", path, flags);
    zx_status_t r;
    if ((r = vfs_validate_flags(flags)) != ZX_OK) {
        return r;
    }
    if ((r = Vfs::Walk(vndir, &vndir, path, &path)) < 0) {
        return r;
    }
    if (r > 0) {
        // remote filesystem, return handle and path through to caller
        *pathout = path;
        return r;
    }

    size_t len = strlen(path);
    fbl::RefPtr<Vnode> vn;

    bool must_be_dir = false;
    if ((r = vfs_name_trim(path, len, &len, &must_be_dir)) != ZX_OK) {
        return r;
    } else if (is_dot_dot(path, len)) {
        return ZX_ERR_INVALID_ARGS;
    }

    if (flags & O_CREAT) {
        if (must_be_dir && !S_ISDIR(mode)) {
            return ZX_ERR_INVALID_ARGS;
        } else if (is_dot(path, len)) {
            return ZX_ERR_INVALID_ARGS;
        }

        if ((r = vndir->Create(&vn, path, len, mode)) < 0) {
            if ((r == ZX_ERR_ALREADY_EXISTS) && (!(flags & O_EXCL))) {
                goto try_open;
            }
            if (r == ZX_ERR_NOT_SUPPORTED) {
                // filesystem may not support create (like devfs)
                // in which case we should still try to open() the file
                goto try_open;
            }
            return r;
        }
        vndir->Notify(path, len, VFS_WATCH_EVT_ADDED);
    } else {
    try_open:
        r = vfs_lookup(fbl::move(vndir), &vn, path, len);
        if (r < 0) {
            return r;
        }
#ifdef __Fuchsia__
        if (!(flags & O_NOREMOTE) && vn->IsRemote() && !vn->IsDevice()) {
            // Opening a mount point: Traverse across remote.
            // Devices are different, even though they also have remotes.  Ignore them.
            *pathout = ".";

            if ((r = Vfs::WaitForRemoteLocked(vn)) != ZX_ERR_PEER_CLOSED) {
                return r;
            }
        }

        flags |= (must_be_dir ? O_DIRECTORY : 0);
#endif
        if ((r = vn->Open(flags)) < 0) {
            return r;
        }
#ifdef __Fuchsia__
        if (vn->IsDevice() && !(flags & O_DIRECTORY)) {
            *pathout = ".";
            r = vn->GetRemote();
            return r;
        }
#endif
        if ((flags & O_TRUNC) && ((r = vn->Truncate(0)) < 0)) {
            return r;
        }
    }
    FS_TRACE(VFS, "VfsOpen: vn=%p\n", vn.get());
    *pathout = "";
    *out = vn;
    return ZX_OK;
}

zx_status_t Vfs::Unlink(fbl::RefPtr<Vnode> vndir, const char* path, size_t len) {
    bool must_be_dir;
    zx_status_t r;
    if ((r = vfs_name_trim(path, len, &len, &must_be_dir)) != ZX_OK) {
        return r;
    } else if (is_dot(path, len)) {
        return ZX_ERR_UNAVAILABLE;
    } else if (is_dot_dot(path, len)) {
        return ZX_ERR_INVALID_ARGS;
    }

    {
#ifdef __Fuchsia__
        fbl::AutoLock lock(&vfs_lock_);
#endif
        r = vndir->Unlink(path, len, must_be_dir);
    }
    if (r != ZX_OK) {
        return r;
    }
    vndir->Notify(path, len, VFS_WATCH_EVT_REMOVED);
    return ZX_OK;
}

#ifdef __Fuchsia__

#define TOKEN_RIGHTS (ZX_RIGHT_DUPLICATE | ZX_RIGHT_TRANSFER)

void Vfs::TokenDiscard(zx::event ios_token) {
    fbl::AutoLock lock(&vfs_lock_);
    if (ios_token) {
        // The token is cleared here to prevent the following race condition:
        // 1) Open
        // 2) GetToken
        // 3) Close + Release Vnode
        // 4) Use token handle to access defunct vnode (or a different vnode,
        //    if the memory for it is reallocated).
        //
        // By cleared the token cookie, any remaining handles to the event will
        // be ignored by the filesystem server.
        ios_token.set_cookie(zx_process_self(), 0);
    }
}

zx_status_t Vfs::VnodeToToken(fbl::RefPtr<Vnode> vn, zx::event* ios_token,
                              zx::event* out) {
    uint64_t vnode_cookie = reinterpret_cast<uint64_t>(vn.get());
    zx_status_t r;

    fbl::AutoLock lock(&vfs_lock_);
    if (ios_token->is_valid()) {
        // Token has already been set for this iostate
        if ((r = ios_token->duplicate(TOKEN_RIGHTS, out) != ZX_OK)) {
            return r;
        }
        return ZX_OK;
    }

    zx::event new_token;
    zx::event new_ios_token;
    if ((r = zx::event::create(0, &new_ios_token)) != ZX_OK) {
        return r;
    } else if ((r = new_ios_token.duplicate(TOKEN_RIGHTS, &new_token) != ZX_OK)) {
        return r;
    } else if ((r = new_ios_token.set_cookie(zx_process_self(), vnode_cookie)) != ZX_OK) {
        return r;
    }
    *ios_token = fbl::move(new_ios_token);
    *out = fbl::move(new_token);
    return ZX_OK;
}

zx_status_t Vfs::TokenToVnode(zx::event token, fbl::RefPtr<Vnode>* out) {
    uint64_t vcookie;
    zx_status_t r;
    if ((r = token.get_cookie(zx_process_self(), &vcookie)) < 0) {
        // TODO(smklein): Return a more specific error code for "token not from this server"
        return ZX_ERR_INVALID_ARGS;
    }

    if (vcookie == 0) {
        // Client closed the channel associated with the token
        return ZX_ERR_INVALID_ARGS;
    }

    *out = fbl::RefPtr<fs::Vnode>(reinterpret_cast<fs::Vnode*>(vcookie));
    return ZX_OK;
}

zx_status_t Vfs::Rename(zx::event token, fbl::RefPtr<Vnode> oldparent,
                        const char* oldname, const char* newname) {
    // Local filesystem
    size_t oldlen = strlen(oldname);
    size_t newlen = strlen(newname);
    bool old_must_be_dir;
    bool new_must_be_dir;
    zx_status_t r;
    if ((r = vfs_name_trim(oldname, oldlen, &oldlen, &old_must_be_dir)) != ZX_OK) {
        return r;
    } else if (is_dot(oldname, oldlen)) {
        return ZX_ERR_UNAVAILABLE;
    } else if (is_dot_dot(oldname, oldlen)) {
        return ZX_ERR_INVALID_ARGS;
    }

    if ((r = vfs_name_trim(newname, newlen, &newlen, &new_must_be_dir)) != ZX_OK) {
        return r;
    } else if (is_dot_or_dot_dot(newname, newlen)) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::RefPtr<fs::Vnode> newparent;
    {
        fbl::AutoLock lock(&vfs_lock_);
        if ((r = TokenToVnode(fbl::move(token), &newparent)) != ZX_OK) {
            return r;
        }

        r = oldparent->Rename(newparent, oldname, oldlen, newname, newlen,
                              old_must_be_dir, new_must_be_dir);
    }
    if (r != ZX_OK) {
        return r;
    }
    oldparent->Notify(oldname, oldlen, VFS_WATCH_EVT_REMOVED);
    newparent->Notify(newname, newlen, VFS_WATCH_EVT_ADDED);
    return ZX_OK;
}

zx_status_t Vfs::Readdir(Vnode* vn, vdircookie_t* cookie,
                         void* dirents, size_t len) {
    fbl::AutoLock lock(&vfs_lock_);
    return vn->Readdir(cookie, dirents, len);
}

zx_status_t Vfs::Link(zx::event token, fbl::RefPtr<Vnode> oldparent,
                      const char* oldname, const char* newname) {
    fbl::AutoLock lock(&vfs_lock_);
    fbl::RefPtr<fs::Vnode> newparent;
    zx_status_t r;
    if ((r = TokenToVnode(fbl::move(token), &newparent)) != ZX_OK) {
        return r;
    }
    // Local filesystem
    size_t oldlen = strlen(oldname);
    size_t newlen = strlen(newname);
    bool old_must_be_dir;
    bool new_must_be_dir;
    if ((r = vfs_name_trim(oldname, oldlen, &oldlen, &old_must_be_dir)) != ZX_OK) {
        return r;
    } else if (old_must_be_dir) {
        return ZX_ERR_NOT_DIR;
    } else if (is_dot(oldname, oldlen)) {
        return ZX_ERR_UNAVAILABLE;
    } else if (is_dot_dot(oldname, oldlen)) {
        return ZX_ERR_INVALID_ARGS;
    }

    if ((r = vfs_name_trim(newname, newlen, &newlen, &new_must_be_dir)) != ZX_OK) {
        return r;
    } else if (new_must_be_dir) {
        return ZX_ERR_NOT_DIR;
    } else if (is_dot_or_dot_dot(newname, newlen)) {
        return ZX_ERR_INVALID_ARGS;
    }

    // Look up the target vnode
    fbl::RefPtr<Vnode> target;
    if ((r = oldparent->Lookup(&target, oldname, oldlen)) < 0) {
        return r;
    }
    r = newparent->Link(newname, newlen, target);
    if (r != ZX_OK) {
        return r;
    }
    newparent->Notify(newname, newlen, VFS_WATCH_EVT_ADDED);
    return ZX_OK;
}

zx_handle_t Vfs::WaitForRemoteLocked(fbl::RefPtr<Vnode> vn) {
    zx_handle_t h = vn->WaitForRemote();

    if (h == ZX_ERR_PEER_CLOSED) {
        printf("VFS: Remote filesystem channel closed, unmounting\n");
        zx::channel c;
        zx_status_t status;
        if ((status = Vfs::UninstallRemoteLocked(vn, &c)) != ZX_OK) {
            return status;
        }
    }

    return h;
}

zx_status_t Vfs::ServeConnection(fbl::unique_ptr<Connection> connection) {
    ZX_DEBUG_ASSERT(connection);

    zx_status_t status = connection->Serve();
    if (status == ZX_OK) {
        RegisterConnection(fbl::move(connection));
    }
    return status;
}

void Vfs::OnConnectionClosedRemotely(Connection* connection) {
    ZX_DEBUG_ASSERT(connection);

    UnregisterAndDestroyConnection(connection);
}

zx_status_t Vfs::ServeDirectory(fbl::RefPtr<fs::Vnode> vn, zx::channel channel) {
    // Make sure the Vnode really is a directory.
    zx_status_t r;
    if ((r = vn->Open(O_DIRECTORY)) != ZX_OK) {
        return r;
    }

    // Tell the calling process that we've mounted the directory.
    if ((r = channel.signal_peer(0, ZX_USER_SIGNAL_0)) != ZX_OK) {
        return r;
    }

    return vn->Serve(this, fbl::move(channel), O_ADMIN);
}

void Vfs::RegisterConnection(fbl::unique_ptr<Connection> connection) {
    // The connection will be destroyed by |UnregisterAndDestroyConnection()|
    connection.release();
}

void Vfs::UnregisterAndDestroyConnection(Connection* connection) {
    delete connection;
}

#endif // ifdef __Fuchsia__

zx_status_t Vfs::Ioctl(fbl::RefPtr<Vnode> vn, uint32_t op, const void* in_buf, size_t in_len,
                       void* out_buf, size_t out_len, size_t* out_actual) {
    switch (op) {
#ifdef __Fuchsia__
    case IOCTL_VFS_WATCH_DIR: {
        if (in_len != sizeof(vfs_watch_dir_t)) {
            return ZX_ERR_INVALID_ARGS;
        }
        const vfs_watch_dir_t* request = reinterpret_cast<const vfs_watch_dir_t*>(in_buf);
        *out_actual = 0;
        return vn->WatchDir(this, request);
    }
    case IOCTL_VFS_MOUNT_FS: {
        if ((in_len != sizeof(zx_handle_t)) || (out_len != 0)) {
            return ZX_ERR_INVALID_ARGS;
        }
        MountChannel h = MountChannel(*reinterpret_cast<const zx_handle_t*>(in_buf));
        *out_actual = 0;
        return Vfs::InstallRemote(vn, fbl::move(h));
    }
    case IOCTL_VFS_MOUNT_MKDIR_FS: {
        size_t namelen = in_len - sizeof(mount_mkdir_config_t);
        const mount_mkdir_config_t* config = reinterpret_cast<const mount_mkdir_config_t*>(in_buf);
        const char* name = config->name;
        if ((in_len < sizeof(mount_mkdir_config_t)) ||
            (namelen < 1) || (namelen > PATH_MAX) || (name[namelen - 1] != 0) ||
            (out_len != 0)) {
            return ZX_ERR_INVALID_ARGS;
        }

        *out_actual = 0;
        return Vfs::MountMkdir(fbl::move(vn), config);
    }
    case IOCTL_VFS_UNMOUNT_NODE: {
        if ((in_len != 0) || (out_len != sizeof(zx_handle_t))) {
            return ZX_ERR_INVALID_ARGS;
        }
        zx_handle_t* h = (zx_handle_t*)out_buf;
        zx::channel c;
        *out_actual = 0;
        zx_status_t s = Vfs::UninstallRemote(vn, &c);
        *h = c.release();
        return s;
    }
    case IOCTL_VFS_UNMOUNT_FS: {
        Vfs::UninstallAll(ZX_TIME_INFINITE);
        *out_actual = 0;
        vn->Ioctl(op, in_buf, in_len, out_buf, out_len, out_actual);
        return ZX_OK;
    }
#endif
    default:
        return vn->Ioctl(op, in_buf, in_len, out_buf, out_len, out_actual);
    }
}

// Starting at vnode vn, walk the tree described by the path string,
// until either there is only one path segment remaining in the string
// or we encounter a vnode that represents a remote filesystem
//
// If a non-negative status is returned, the vnode at 'out' has been acquired.
// Otherwise, no net deltas in acquires/releases occur.
zx_status_t Vfs::Walk(fbl::RefPtr<Vnode> vn, fbl::RefPtr<Vnode>* out,
                      const char* path, const char** pathout) {
    zx_status_t r;

    for (;;) {
        while (path[0] == '/') {
            // discard extra leading /s
            path++;
        }
        if (path[0] == 0) {
            // convert empty initial path of final path segment to "."
            path = ".";
        }
#ifdef __Fuchsia__
        if (vn->IsRemote() && !vn->IsDevice()) {
            // remote filesystem mount, caller must resolve
            // devices are different, so ignore them even though they can have vn->remote
            r = Vfs::WaitForRemoteLocked(vn);
            if (r != ZX_ERR_PEER_CLOSED) {
                if (r >= 0) {
                    *out = vn;
                    *pathout = path;
                }
                return r;
            }
        }
#endif

        const char* nextpath = strchr(path, '/');
        bool additional_segment = false;
        if (nextpath != nullptr) {
            const char* end = nextpath;
            while (*end != '\0') {
                if (*end != '/') {
                    additional_segment = true;
                    break;
                }
                end++;
            }
        }
        if (additional_segment) {
            // path has at least one additional segment
            // traverse to the next segment
            size_t len = nextpath - path;
            nextpath++;
            if ((r = vfs_lookup(fbl::move(vn), &vn, path, len)) < 0) {
                return r;
            }
            path = nextpath;
        } else {
            // final path segment, we're done here
            *out = vn;
            *pathout = path;
            return ZX_OK;
        }
    }
}

} // namespace fs
