#pragma once
#include <string>

// ── Android Storage Access Framework bridge ────────────────────────────
// Replaces the desktop pfd (portable_file_dialogs) calls that silently
// no-op'd on Android. All three functions BLOCK the calling thread until
// the user picks or cancels in the system file UI — only call from a
// dedicated worker thread, never from the main render loop, or the
// frame will hang for as long as the picker is open.
//
// The Java side internally marshals the SAF intent dispatch to the UI
// thread, so calling from the native game thread does not deadlock —
// but it does freeze rendering until the user dismisses the picker.
namespace android_saf {

    // Open a system file picker, copy the chosen file into the app's
    // cache directory, and return the resulting cache path. Existing
    // std::ifstream / fopen code can read the returned path normally.
    // Returns "" on cancel / failure.
    //   mimeFilter examples: "application/json", "audio/*", "" (any)
    std::string pickFileForReadBlocking(const std::string& mimeFilter);

    // Open the system folder picker. Returns the SAF tree URI as a
    // string (e.g. "content://com.android.externalstorage.documents/...").
    // NOT a filesystem path — callers that try to fopen() it will fail;
    // this is by design and is what surfaces upstream so we can teach
    // recorder/etc. to write through ContentResolver later. Returns ""
    // on cancel.
    std::string pickFolderBlocking();

    // Open the system "save as" picker pre-filled with suggestedName.
    // On confirm, the contents of sourceCachePath are copied byte-for-
    // byte into the user-chosen destination. The caller is responsible
    // for writing the file to sourceCachePath BEFORE calling this.
    // Returns true on success, false on cancel / IO error.
    bool saveFileBlocking(const std::string& suggestedName,
                          const std::string& sourceCachePath);
}
