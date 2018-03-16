// g++ -std=c++11 -Og -g3 -Wall -Werror -I/usr/local/include/wpiutil -I/usr/local/include/ntcore -L/usr/local/lib -o ntwatch ntwatch.c -lntcore -lwpiutil -lpthread
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <ntcore.h>

int main(int argc, char **argv) {
    const char *myname = "Vision";
    const char *robot_host = "10.15.19.2";
    const char *valname = "MyValue";
    NT_Inst inst = NT_GetDefaultInstance();

    NT_SetNetworkIdentity(inst, myname, strlen(myname));
    NT_StartClient(inst, robot_host, NT_DEFAULT_PORT);
    while (!NT_IsConnected(inst)) {
        printf("Waiting to connect...\n");
        sleep(5);
    }
    printf("Connected\n");

    NT_EntryListenerPoller poller = NT_CreateEntryListenerPoller(inst);
    NT_Entry entry = NT_GetEntry(inst, valname, strlen(valname));
    int poll_flags = NT_NOTIFY_IMMEDIATE | NT_NOTIFY_NEW | NT_NOTIFY_UPDATE;
    NT_AddPolledEntryListenerSingle(poller, entry, poll_flags);
    printf("Listener ready for changes to \"%s\"\n", valname);

    // TODO Initial check of value?
    
    size_t len;
    double timeout = 5.0;
    NT_Bool timed_out;
    struct NT_EntryNotification *notification;
    while (1) {
        printf("Polling for Network Table update...\n");
        notification = NT_PollEntryListenerTimeout(poller, &len, timeout, &timed_out);
        if (timed_out) {
            printf("No update received\n");
            continue;
        }
        double vDouble;
        uint64_t timestamp;
        if (!NT_GetValueDouble(&notification->value, &timestamp, &vDouble)) {
            printf("Failed to get double value\n");
            continue;
        }
        int value = (int)(vDouble + 0.5);

        printf("UPDATE[%" PRIu64 "]: %s ==> %d\n", timestamp, notification->name.str, value);
        NT_DisposeEntryNotification(notification);
    }
    NT_DestroyEntryListenerPoller(poller);
}
