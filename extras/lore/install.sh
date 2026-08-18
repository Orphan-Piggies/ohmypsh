#!/bin/bash

# Fıstıq rəngləri
GREEN='\033[0;32m'
BROWN='\033[0;33m'
RESET='\033[0m'

echo -e "${GREEN}[*] oh-my-pistachio quraşdırılır...${RESET}"
sleep 1
echo -e "${BROWN}[*] Kodlar qovrulur...${RESET}"
sleep 1
echo -e "${GREEN}[+] Uğurla quraşdırıldı! Sistem indi duzludur.${RESET}"

# Yeni Prompt funksiyası
PS1="(🥜 pistachio) \u@\h:\w$ "

# Səhv komanda işləyəndə işə düşən funksiya
command_not_found_handle() {
    echo -e "${BROWN}[!] XƏTA: Bu fıstıq qabığını aça bilmədik!${RESET}"
    echo -e "${GREEN}[i] Məsləhət: Komandanı yenidən yazmazdan əvvəl barmaqlarınla zərbə vur.${RESET}"
    return 127
}

