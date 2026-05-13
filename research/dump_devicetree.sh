#!/bin/bash
set +e
walk() {
  local d=$1
  if [ -f "$d/compatible" ]; then
    local c=$(tr '\0' ' ' < "$d/compatible")
    case "$c" in
      *usb*|*phy*|*ccu*|*xhci*|*ehci*|*ohci*|*clock-controller*) 
        echo "==== $d ===="
        echo "compatible: $c"
        for f in reg clocks resets phys clock-names reset-names phy-names interrupts status; do
          if [ -f "$d/$f" ]; then
            printf "%s: " "$f"
            case "$f" in
              *-names|status) tr '\0' ' ' < "$d/$f"; echo ;;
              *) xxd -c 32 "$d/$f" | head -2 ;;
            esac
          fi
        done
        echo
        ;;
    esac
  fi
  for sub in "$d"/*; do
    [ -d "$sub" ] && walk "$sub"
  done
}
walk /proc/device-tree
