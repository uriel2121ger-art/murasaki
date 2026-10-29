#pragma once

#include "testutil.h"

#include <QFile>
#include <QString>

// A stand-in for SteamCMD: a shell script that behaves like the parts Murasaki drives. Its state (the "token") lives in
// $FAKE_STEAMCMD_DIR, so a test decides what is already signed in. The script reads what it is told on its terminal.
//
//   arguments:   +force_install_dir DIR   where items go (DIR/steamapps/workshop/content/<app>/<id>)
//                +login USER              signs in (prompts when there is no token)
//                +workshop_download_item APP ID
//                +logout, +quit
//   environment: FAKE_GUARD=1      ask for a Steam Guard code after the password (the right one is 12345)
//                FAKE_MOBILE=1     ask to confirm on the phone instead (it is confirmed after 0.4 s)
//                FAKE_PASSWORD     the password it accepts (default "secret")
//                FAKE_FAIL_ITEM    this item id fails with "Access Denied"
//                FAKE_SLOW=1       a pause between progress lines, to cancel in the middle
//                FAKE_EXPIRED=1    the cached token is refused during a download: it prompts for the password
//                FAKE_SILENT_ITEM  this item id ends without any result line
//                FAKE_CHILD=1      a download starts a helper process (its pid goes to $FAKE_STEAMCMD_DIR/childpid), as
//                                  the real steamcmd does; it must die with the group
//                FAKE_CHATTER=1    after the password it prints a few dots without a newline before going on
//   every start is appended to $FAKE_STEAMCMD_DIR/calls (the arguments), and what it reads as a password to
//   $FAKE_STEAMCMD_DIR/typed (so a test can check what arrived, never the console output).
namespace fakesteamcmd {

inline QString write(const QString &dir)
{
    const QString path = dir + QStringLiteral("/steamcmd");
    testutil::writeFile(path, R"SH(#!/bin/sh
D="${FAKE_STEAMCMD_DIR:?}"
echo "$*" >> "$D/calls"
install=""
user=""
sleep_for() { if [ -n "$FAKE_SLOW" ]; then sleep 1; else sleep 0.05; fi; }
login() {
    user="$1"
    printf "Redirecting stderr to '%s/logs/stderr.txt'\r\n" "$D"
    printf "Logging in user '%s' to Steam Public..." "$user"
    if [ -f "$D/token" ] && [ "$(cat "$D/token")" = "$user" ] && [ -z "$FAKE_EXPIRED" ]; then
        printf "OK\r\nWaiting for client config...OK\r\nWaiting for user info...OK\r\n"
        return 0
    fi
    printf "\r\n"
    printf "password: "
    IFS= read -r pw
    echo "$pw" >> "$D/typed"
    printf "\r\n"
    if [ -n "$FAKE_CHATTER" ]; then
        for i in 1 2 3 4; do printf "."; sleep 0.1; done
    fi
    if [ "$pw" != "${FAKE_PASSWORD:-secret}" ]; then
        printf "Logging in user '%s' to Steam Public...FAILED (Invalid Password)\r\n" "$user"
        exit 5
    fi
    if [ -n "$FAKE_GUARD" ]; then
        printf "This computer has not been authenticated for your account using Steam Guard.\r\n"
        printf "Please check your email for the message from Steam, and enter the Steam Guard\r\ncode from that message.\r\n"
        printf "Steam Guard code: "
        IFS= read -r code
        echo "$code" >> "$D/typed"
        printf "\r\n"
        if [ "$code" != "12345" ]; then
            printf "Logging in user '%s' to Steam Public...FAILED (Two-factor code mismatch)\r\n" "$user"
            exit 5
        fi
    elif [ -n "$FAKE_MOBILE" ]; then
        printf "Please confirm the login in the Steam Mobile app on your phone.\r\n"
        sleep 0.4
        printf "Waiting for confirmation...OK\r\n"
    fi
    printf "Waiting for client config...OK\r\nWaiting for user info...OK\r\n"
    echo "$user" > "$D/token"
}
download() {
    app="$1"; id="$2"
    if [ -f "$D/token" ] && [ -n "$FAKE_EXPIRED" ]; then
        printf "password: "
        IFS= read -r pw
        exit 5
    fi
    if [ -n "$FAKE_CHILD" ]; then
        sleep 60 &
        echo $! > "$D/childpid"
    fi
    printf "Downloading item %s ...\r\n" "$id"
    if [ "$id" = "$FAKE_SILENT_ITEM" ]; then exit 0; fi
    for p in 10 40 70; do
        printf "Update state (0x61) downloading, progress: %s.00 (%s / 1000)\r" "$p" "$((p * 10))"
        sleep_for
    done
    if [ "$id" = "$FAKE_FAIL_ITEM" ]; then
        printf "\r\nERROR! Download item %s failed (Access Denied).\r\n" "$id"
        exit 8
    fi
    mkdir -p "$install/steamapps/workshop/content/$app/$id"
    printf '{"type":"video","file":"clip.mp4","title":"Fake %s"}' "$id" > "$install/steamapps/workshop/content/$app/$id/project.json"
    printf "fake" > "$install/steamapps/workshop/content/$app/$id/clip.mp4"
    printf "Update state (0x61) downloading, progress: 100.00 (1000 / 1000)\r\n"
    printf "Success. Downloaded item %s to \"%s/steamapps/workshop/content/%s/%s\" (1000 bytes)\r\n" "$id" "$install" "$app" "$id"
}
while [ $# -gt 0 ]; do
    case "$1" in
    +force_install_dir) install="$2"; shift 2 ;;
    +login) login "$2"; shift 2
        # interactive: with no +quit left the console waits for commands
        case " $* " in *" +quit "*) ;; *) printf "Steam>"; IFS= read -r cmd; exit 0 ;; esac ;;
    +workshop_download_item) download "$2" "$3"; shift 3 ;;
    +logout) printf "Logging off...OK\r\n"; rm -f "$D/token"; shift ;;
    +quit) shift ;;
    *) shift ;;
    esac
done
exit 0
)SH");
    QFile(path).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

} // namespace fakesteamcmd
