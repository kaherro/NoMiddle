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
        for (int i = 0; i < 30 && running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::cout << "[RETRY_WORKER] Stopped\n";
}