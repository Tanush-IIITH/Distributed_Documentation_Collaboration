#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void test_message_building() {
    printf("Testing message building...\n");
    
    // Test HELLO_CLIENT
    const char *fields1[] = {MSG_HELLO_CLIENT, "john_doe"};
    char *msg1 = protocol_build_message(fields1, 2);
    assert(msg1 != NULL);
    assert(strcmp(msg1, "HELLO_CLIENT|john_doe\n") == 0);
    printf("  ✓ HELLO_CLIENT message: %s", msg1);
    free(msg1);
    
    // Test HELLO_SS
    const char *fields2[] = {MSG_HELLO_SS, "192.168.1.10", "8001", "192.168.1.10", "8002"};
    char *msg2 = protocol_build_message(fields2, 5);
    assert(msg2 != NULL);
    assert(strcmp(msg2, "HELLO_SS|192.168.1.10|8001|192.168.1.10|8002\n") == 0);
    printf("  ✓ HELLO_SS message: %s", msg2);
    free(msg2);
    
    // Test CREATE
    const char *fields3[] = {MSG_CREATE, "newfile.txt"};
    char *msg3 = protocol_build_message(fields3, 2);
    assert(msg3 != NULL);
    assert(strcmp(msg3, "CREATE|newfile.txt\n") == 0);
    printf("  ✓ CREATE message: %s", msg3);
    free(msg3);
    
    // Test REQ_LOC
    const char *fields4[] = {MSG_REQ_LOC, "READ", "document.txt"};
    char *msg4 = protocol_build_message(fields4, 3);
    assert(msg4 != NULL);
    assert(strcmp(msg4, "REQ_LOC|READ|document.txt\n") == 0);
    printf("  ✓ REQ_LOC message: %s", msg4);
    free(msg4);
    
    printf("Message building tests passed!\n\n");
}

void test_message_parsing() {
    printf("Testing message parsing...\n");
    
    // Test parsing HELLO_CLIENT
    ProtocolMessage msg1;
    int result = protocol_parse_message("HELLO_CLIENT|john_doe\n", &msg1);
    assert(result == 0);
    assert(msg1.field_count == 2);
    assert(strcmp(msg1.fields[0], MSG_HELLO_CLIENT) == 0);
    assert(strcmp(msg1.fields[1], "john_doe") == 0);
    printf("  ✓ Parsed HELLO_CLIENT: %s, %s\n", msg1.fields[0], msg1.fields[1]);
    protocol_free_message(&msg1);
    
    // Test parsing CREATE_FILE
    ProtocolMessage msg2;
    result = protocol_parse_message("CREATE_FILE|file.txt|owner_user\n", &msg2);
    assert(result == 0);
    assert(msg2.field_count == 3);
    assert(strcmp(msg2.fields[0], MSG_CREATE_FILE) == 0);
    assert(strcmp(msg2.fields[1], "file.txt") == 0);
    assert(strcmp(msg2.fields[2], "owner_user") == 0);
    printf("  ✓ Parsed CREATE_FILE: %s, %s, %s\n", msg2.fields[0], msg2.fields[1], msg2.fields[2]);
    protocol_free_message(&msg2);
    
    // Test parsing REQ_WRITE_LOCK
    ProtocolMessage msg3;
    result = protocol_parse_message("REQ_WRITE_LOCK|user|file.txt|2\n", &msg3);
    assert(result == 0);
    assert(msg3.field_count == 4);
    assert(strcmp(msg3.fields[0], MSG_REQ_WRITE_LOCK) == 0);
    assert(strcmp(msg3.fields[3], "2") == 0);
    printf("  ✓ Parsed REQ_WRITE_LOCK: %s, %s, %s, %s\n", 
           msg3.fields[0], msg3.fields[1], msg3.fields[2], msg3.fields[3]);
    protocol_free_message(&msg3);
    
    printf("Message parsing tests passed!\n\n");
}

void test_error_messages() {
    printf("Testing error messages...\n");
    
    // Test building error with code
    char *err1 = protocol_build_error(ERR_FILE_NOT_FOUND, NULL);
    assert(err1 != NULL);
    assert(strstr(err1, "ERR") != NULL);
    assert(strstr(err1, "404") != NULL);
    printf("  ✓ Error 404: %s", err1);
    free(err1);
    
    // Test building error with custom message
    char *err2 = protocol_build_error(ERR_PERMISSION_DENIED, "You cannot access this file.");
    assert(err2 != NULL);
    assert(strcmp(err2, "ERR|403|You cannot access this file.\n") == 0);
    printf("  ✓ Error 403: %s", err2);
    free(err2);
    
    // Test parsing error message
    ProtocolMessage err_msg;
    protocol_parse_message("ERR|423|Sentence is locked by another user.\n", &err_msg);
    assert(protocol_is_error(&err_msg) == 1);
    assert(protocol_get_error_code(&err_msg) == 423);
    printf("  ✓ Parsed error: code=%d, message=%s\n", 
           protocol_get_error_code(&err_msg), err_msg.fields[2]);
    protocol_free_message(&err_msg);
    
    printf("Error message tests passed!\n\n");
}

void test_ok_messages() {
    printf("Testing OK messages...\n");
    
    // Test simple OK
    char *ok1 = protocol_build_ok(NULL);
    assert(ok1 != NULL);
    assert(strcmp(ok1, "OK\n") == 0);
    printf("  ✓ Simple OK: %s", ok1);
    free(ok1);
    
    // Test OK with message
    char *ok2 = protocol_build_ok("File created successfully.");
    assert(ok2 != NULL);
    assert(strcmp(ok2, "OK|File created successfully.\n") == 0);
    printf("  ✓ OK with message: %s", ok2);
    free(ok2);
    
    // Test OK_LOC
    const char *fields[] = {RESP_OK_LOC, "file.txt", "192.168.1.10", "8002"};
    char *ok3 = protocol_build_message(fields, 4);
    assert(ok3 != NULL);
    assert(strcmp(ok3, "OK_LOC|file.txt|192.168.1.10|8002\n") == 0);
    printf("  ✓ OK_LOC: %s", ok3);
    free(ok3);
    
    printf("OK message tests passed!\n\n");
}

void test_validation() {
    printf("Testing message validation...\n");
    
    // Valid message
    assert(protocol_validate_message("HELLO_CLIENT|user\n") == 1);
    printf("  ✓ Valid message recognized\n");
    
    // Invalid - no terminator
    assert(protocol_validate_message("HELLO_CLIENT|user") == 0);
    printf("  ✓ Invalid message (no terminator) rejected\n");
    
    // Invalid - NULL
    assert(protocol_validate_message(NULL) == 0);
    printf("  ✓ NULL message rejected\n");
    
    printf("Validation tests passed!\n\n");
}

void test_specific_commands() {
    printf("Testing specific command formats...\n");
    
    // ADDACCESS
    const char *addaccess[] = {MSG_ADDACCESS, "file.txt", "jane_doe", PERM_READ};
    char *msg = protocol_build_message(addaccess, 4);
    assert(strcmp(msg, "ADDACCESS|file.txt|jane_doe|R\n") == 0);
    printf("  ✓ ADDACCESS: %s", msg);
    free(msg);
    
    // WRITE_DATA
    const char *write_data[] = {MSG_WRITE_DATA, "5", "awesome"};
    msg = protocol_build_message(write_data, 3);
    assert(strcmp(msg, "WRITE_DATA|5|awesome\n") == 0);
    printf("  ✓ WRITE_DATA: %s", msg);
    free(msg);
    
    // GET_STATS
    const char *get_stats[] = {MSG_GET_STATS, "document.txt"};
    msg = protocol_build_message(get_stats, 2);
    assert(strcmp(msg, "GET_STATS|document.txt\n") == 0);
    printf("  ✓ GET_STATS: %s", msg);
    free(msg);
    
    // OK_STATS (response)
    const char *ok_stats[] = {RESP_OK_STATS, "doc.txt", "150", "823", "1024", "1729785000", "1729784500"};
    msg = protocol_build_message(ok_stats, 7);
    assert(msg != NULL);
    printf("  ✓ OK_STATS: %s", msg);
    free(msg);
    
    // CHECKPOINT
    const char *checkpoint[] = {MSG_REQ_CHECKPOINT, "user", "file.txt", "version1"};
    msg = protocol_build_message(checkpoint, 4);
    assert(strcmp(msg, "REQ_CHECKPOINT|user|file.txt|version1\n") == 0);
    printf("  ✓ REQ_CHECKPOINT: %s", msg);
    free(msg);
    
    printf("Specific command tests passed!\n\n");
}

void test_complete_flows() {
    printf("Testing complete protocol flows...\n");
    
    // CREATE flow
    printf("  Testing CREATE flow:\n");
    const char *create_req[] = {MSG_CREATE, "newfile.txt"};
    char *msg = protocol_build_message(create_req, 2);
    printf("    Client->NS: %s", msg);
    free(msg);
    
    const char *create_file[] = {MSG_CREATE_FILE, "newfile.txt", "john_doe"};
    msg = protocol_build_message(create_file, 3);
    printf("    NS->SS: %s", msg);
    free(msg);
    
    const char *create_ok[] = {RESP_OK_CREATE, "File created."};
    msg = protocol_build_message(create_ok, 2);
    printf("    SS->NS: %s", msg);
    free(msg);
    
    msg = protocol_build_ok("File 'newfile.txt' created successfully.");
    printf("    NS->Client: %s", msg);
    free(msg);
    
    // WRITE flow
    printf("\n  Testing WRITE flow:\n");
    const char *req_loc[] = {MSG_REQ_LOC, "WRITE", "file.txt"};
    msg = protocol_build_message(req_loc, 3);
    printf("    Client->NS: %s", msg);
    free(msg);
    
    const char *ok_loc[] = {RESP_OK_LOC, "file.txt", "192.168.1.10", "8002"};
    msg = protocol_build_message(ok_loc, 4);
    printf("    NS->Client: %s", msg);
    free(msg);
    
    const char *lock_req[] = {MSG_REQ_WRITE_LOCK, "user", "file.txt", "0"};
    msg = protocol_build_message(lock_req, 4);
    printf("    Client->SS: %s", msg);
    free(msg);
    
    const char *locked[] = {RESP_OK_LOCKED, "0", "This is the sentence."};
    msg = protocol_build_message(locked, 3);
    printf("    SS->Client: %s", msg);
    free(msg);
    
    const char *write[] = {MSG_WRITE_DATA, "2", "new_word"};
    msg = protocol_build_message(write, 3);
    printf("    Client->SS: %s", msg);
    free(msg);
    
    const char *etirw[] = {MSG_ETIRW};
    msg = protocol_build_message(etirw, 1);
    printf("    Client->SS: %s", msg);
    free(msg);
    
    const char *write_done[] = {RESP_OK_WRITE_DONE, "Write successful."};
    msg = protocol_build_message(write_done, 2);
    printf("    SS->Client: %s", msg);
    free(msg);
    
    printf("\nComplete flow tests passed!\n\n");
}

int main() {
    printf("===========================================\n");
    printf("     Protocol Implementation Tests\n");
    printf("===========================================\n\n");
    
    test_message_building();
    test_message_parsing();
    test_error_messages();
    test_ok_messages();
    test_validation();
    test_specific_commands();
    test_complete_flows();
    
    printf("===========================================\n");
    printf("     All Tests Passed Successfully! ✓\n");
    printf("===========================================\n");
    
    return 0;
}
