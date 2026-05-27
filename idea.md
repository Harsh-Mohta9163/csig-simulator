# **Protocol Architecture: Hybrid Congestion Control & SmartNIC Scheduling V3**

## **1\. Sender-Side Architecture: FASTFLOW \+ EQDS \+ MCC**

The sender-side protocol is a hybrid engine designed to prevent data center Incast while aggressively maintaining throughput for large tensors. It requires the sender to satisfy a strict receiver-granted credit threshold before transmitting, effectively neutralizing Incast at the edge. Concurrently, it employs Message-Level Congestion Control (MCC). Instead of reacting purely to packet-level micro-bursts (ECN marks or RTT spikes), the algorithm evaluates the health of the overarching logical message. If the message is arriving on schedule, it actively overrides packet-level throttling, preventing unnecessary deceleration.

`// State Variables per Message`  
`acked, bytes_ignored = 0`  
`receiver_credits = 0`  
`initial_burst_bytes = INITIAL_WINDOW_SIZE // e.g., 1 BDP`

`procedure CONGESTION_LOOP_LOGIC(p)`  
    `acked += p.size`  
    `bytes_ignored += p.size`  
      
    `// Accumulate credits explicitly granted by the receiver's NIC`  
    `if p.has_credits then`  
        `receiver_credits += p.granted_credits`  
    `end if`  
      
    `if p.is_ack then`  
        `if bytes_ignored < bytes_to_ignore then`   
            `return`   
        `end if`  
          
        `can_decrease = wait_to_decrease(p)`  
        `adp = quick_adapt(p)`  
        `finc = fast_increase(p)`  
        `if adp or finc then`   
            `return`   
        `end if`

        `// MCC: Evaluate if the overarching message is meeting its target delivery rate`  
        `message_is_healthy = evaluate_message_health(p.msg_id)`  
          
        `// Congestion Loop Logic with MCC overrides`  
        `if p.ecn and p.rtt <= trtt and can_decrease and not message_is_healthy then`  
            `fair_decrease(p)`  
        `else if p.ecn and p.rtt > trtt and can_decrease and not message_is_healthy then`  
            `multiplicative_decrease(p)`  
        `else if not p.ecn and p.rtt > trtt then`  
            `if message_is_healthy then`  
                `multiplicative_increase(p) // MCC dictates ignoring transient micro-delays`  
            `else`  
                `fair_increase(p)`  
            `end if`  
        `else if not p.ecn and p.rtt <= trtt then`  
            `multiplicative_increase(p)`  
        `end if`  
          
    `else if p.is_trimmed or p.timeout_triggered then`  
        `cwnd -= p.size`  
        `trigger_qa = true`  
        `retransmit_packet(p)`  
        `if bytes_ignored >= bytes_to_ignore then`  
            `quick_adapt(p)`  
        `end if`  
    `end if`  
      
    `cwnd = max(min(cwnd, bdp), mtu)`  
`end procedure`

`procedure SEND_DATA(message)`  
    `while message.has_unsent_data() do`  
          
        `has_cwnd_space = bytes_in_flight() < cwnd`  
        `has_receiver_permission = (initial_burst_bytes > 0) or (receiver_credits > 0)`  
          
        `if has_cwnd_space and has_receiver_permission then`  
            `p = create_packet(message)`  
            `transmit(p)`  
              
            `// Consume the appropriate edge-network transmission allowance`  
            `if initial_burst_bytes > 0 then`  
                `initial_burst_bytes -= p.size`  
            `else`  
                `receiver_credits -= p.size`  
            `end if`  
        `else`  
            `wait_for_network_state_change()`  
        `end if`  
    `end while`  
`end procedure`

`procedure evaluate_message_health(msg_id)`  
    `msg_state = retrieve_state(msg_id)`  
    `active_time = current_time - msg_state.start_time`  
    `expected_bytes = active_time * target_bandwidth`  
      
    `// If delivery is within the acceptable tolerance bounds (e.g., 85%), consider it healthy`  
    `if msg_state.acked >= (expected_bytes * tolerance_factor) then`  
        `return true`    
    `else`  
        `return false`   
    `end if`  
`end procedure`

## **2\. Receiver-Side Architecture: SmartNIC Co-Flow Scheduling**

Traditional receiver-driven protocols calculate and distribute credits via a software Hypervisor, which introduces microsecond-level latency and operates agnostically of overarching application workloads. In contrast, this design shifts the credit scheduler directly into the hardware of a SmartNIC (e.g., an NVIDIA BlueField or programmable IPU). The SmartNIC natively tracks the synchronization state of distributed All-Reduce jobs in its onboard SRAM. By intercepting incoming packets at line-rate, the NIC can autonomously halt credits to fast senders and flood priority credits to straggling senders, ensuring the entire Co-Flow finishes simultaneously without relying on a centralized software orchestrator.

`// Tracked in the Receiver SmartNIC's high-speed SRAM`  
`active_coflows = Map<CoFlow_ID, CoFlow_State>`

`procedure ON_PACKET_RECEIVE(p)`  
    `// 1. Update the overarching All-Reduce job state natively in hardware`  
    `job_state = active_coflows[p.coflow_id]`  
    `job_state.bytes_received[p.sender_id] += p.size`  
      
    `// 2. Identify structural imbalances in the Co-Flow delivery`  
    `max_progress = max(job_state.bytes_received)`  
    `my_progress = job_state.bytes_received[p.sender_id]`  
      
    `credits_to_grant = 0`  
      
    `// 3. Asymmetric Credit Allocation Algorithm`  
    `if my_progress == max_progress and has_stragglers(job_state) then`  
        `// ACTION: This sender is leading the pack.`   
        `// Starve them of credits to pause their transmission and clear spine switch queues.`  
        `credits_to_grant = 0`   
          
    `else if (max_progress - my_progress) > STRAGGLER_GAP_THRESHOLD then`  
        `// ACTION: This sender is severely lagging behind the collective.`  
        `// Flood them with maximum priority credits to rapidly close the gap.`  
        `credits_to_grant = calculate_priority_credits()`  
          
    `else`  
        `// ACTION: Sender is grouped tightly within the median.`   
        `// Issue standard fair-share link capacity credits.`  
        `credits_to_grant = calculate_standard_fair_share()`  
    `end if`  
      
    `// 4. Piggyback the dynamically calculated credits onto the hardware-generated ACK`  
    `ack = create_hardware_ack(p)`  
    `ack.granted_credits = credits_to_grant`  
    `transmit_from_hardware(ack)`  
`end procedure`

## **3\. References**

* **Handley et al., "NDP: Receiver-Driven Datacenter Traffic Management" (SIGCOMM) & "EQDS"**  
  *Supports:* The core mechanism of mitigating edge-network Incast using explicit receiver credits and initial blind-send allowances.  
* **"Aeolus: A Building Block for High-Performance Deep Learning Networks" (SIGCOMM)**  
  *Supports:* The Message-Level CC (MCC) strategy   
* **Chowdhury et al., "Efficient CoFlow Scheduling with Varys" (SIGCOMM)**  
  *Supports:* The mathematical foundation for prioritizing the completion of an All-Reduce flow-group over the speed of individual flows.  
* **"HotCocoa: Hardware Congestion Control Abstractions" & "SCENIC: Stream Computation-Enhanced SmartNIC"**  
  *Supports:* Validates that modern programmable SmartNICs are physically capable of executing custom, stateful congestion control logic and flow-scheduling at line-rate.