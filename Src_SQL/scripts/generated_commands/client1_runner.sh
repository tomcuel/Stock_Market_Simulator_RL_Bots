#!/usr/bin/env bash
cd '/Users/tomcuel/Documents/Informatique-Code/GitHub/Stock_Market_Trading_Simulator/Src_SQL'
while IFS= read -r command; do
    echo "${command}"
    sleep $(awk -v a='1.0' -v b='0.2' -v seed="$$${RANDOM}" 'BEGIN{srand(seed); printf "%.3f", a + (b > 0 ? rand() * b : 0)}')
done < '/Users/tomcuel/Documents/Informatique-Code/GitHub/Stock_Market_Trading_Simulator/Src_SQL/scripts/generated_commands/client1_commands.txt' | ./client_account.x 'Client1' 'vDvi8HiJNy7Z' 2>&1 | tee '/Users/tomcuel/Documents/Informatique-Code/GitHub/Stock_Market_Trading_Simulator/Src_SQL/scripts/logs/client1.log'
