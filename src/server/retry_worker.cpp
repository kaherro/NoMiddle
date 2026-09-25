#include "retry_worker.h"
#include "message_delivery.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <ctime>

void start_retrying_worker(db_manager &db, std::atomic<bool> &running) {
    std::cout << "[RETRY_WORKER] Started\n";
    while(running) {
        std::vector<db_manager::message> pending_messages = db.get_pending_messages(); 
        if(pending_messages.empty()) {
            std::cout << "[RETRY_WORKER] No pending messages left\n"; 
        }
        std::vector<db_manager::message> pending_edits = db.get_pending_edits();
        if(pending_edits.empty()) {
            std::cout << "[RETRY_WORKER] No pending edits left\n";
        }
        std::vector<db_manager::group_update> pending_gu = db.get_pending_group_updates();
        if(pending_gu.empty()) {
            std::cout << "[RETRY_WORKER] No pending group updates left\n";
        }
        for(auto u : pending_gu) {
            if(retry_group_update(db, u)) {
                std::cout << "[RETRY_WORKER] Group update (group " << u.group_id
                        << " -> " << u.member_id << ") delivered\n";
            }
            else {
                std::cout << "[RETRY_WORKER] Group update (group " << u.group_id
                        << " -> " << u.member_id << ") is still pending\n";
            }
        }
        std::vector<db_manager::message> pending_deletes = db.get_pending_deletes();
        if(pending_deletes.empty()) {
            std::cout << "[RETRY_WORKER] No pending deletes left\n";
        }
        int64_t current_time = static_cast<int64_t>(std::time(nullptr));
        for(auto msg : pending_messages) {
            if(retry_deliver_message(db, msg)) {
                std::cout << "[RETRY_WORKER] Message " << msg.message_id << " successfully delivered\n"; 
            }
            else {
                if(current_time - msg.timestamp > 86400) {
                    db.mark_failed(msg.message_id); 
                    std::cout << "[RETRY_WORKER] Message " << msg.message_id << " now is nomore in retrying_worker\n"; 
                }
                std::cout << "[RETRY_WORKER] " << msg.recipient_id << " is still offline\n"; 
            }
        }
        for(auto msg : pending_edits) {
            if(retry_message_edit(db, msg)) {
                std::cout << "[RETRY_WORKER] Edit of message " << msg.message_id << " successfully delivered\n"; 
            }
            else {
                if(msg.edited_at != 0 && current_time - msg.edited_at > 86400) {
                    db.mark_edit_failed(msg.message_id); 
                    std::cout << "[RETRY_WORKER] Edit of message " << msg.message_id << " now is nomore in retrying_worker\n"; 
                }
                std::cout << "[RETRY_WORKER] Edit of message " << msg.message_id << " for " << msg.recipient_id << " is still pending\n"; 
            }
        }
        for(auto msg : pending_deletes) {
            if(retry_message_delete(db, msg)) {
                std::cout << "[RETRY_WORKER] Delete of message " << msg.message_id << " successfully delivered\n"; 
            }
            else {
                if(msg.deleted_at != 0 && current_time - msg.deleted_at > 86400) {
                    db.mark_delete_failed(msg.message_id); 
                    std::cout << "[RETRY_WORKER] Delete of message " << msg.message_id << " now is nomore in retrying_worker\n"; 
                }
                std::cout << "[RETRY_WORKER] Delete of message " << msg.message_id << " for " << msg.recipient_id << " is still pending\n"; 
            }
        }
        for (int i = 0; i < 30 && running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::cout << "[RETRY_WORKER] Stopped\n";
}