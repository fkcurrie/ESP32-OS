#!/usr/bin/env python3
"""TTC Transit Live Board Daemon for Waveshare ESP32-C6 LCD.

Runs on amd-r9 and continuously pushes live Subway and Bus prediction data
to the desk screen.
"""

import sys
import os
import time
import datetime
import urllib.request
import xml.etree.ElementTree as ET
import subprocess

# Config
PORT = "/dev/ttyACM0"
SCREEN_CTL = "/home/fcurrie/Projects/small-led-screen/scripts/screen_ctl.py"

def get_subway_predictions():
    url = 'https://gtfsrt.ttc.ca/trips/subway?format=text'
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        with urllib.request.urlopen(req, timeout=5.0) as r:
            text = r.read().decode('utf-8', errors='ignore')
    except Exception as e:
        print(f"Subway fetch error: {e}", file=sys.stderr)
        return []
    
    now = int(time.time())
    arrivals = []
    
    # Lightweight textproto block parser
    entities = text.split("entity {")
    for entity in entities[1:]:
        if 'route_id: "1"' not in entity:
            continue
        
        updates = entity.split("stop_time_update {")
        for update in updates[1:]:
            if 'stop_id: "13795"' in update: # Eglinton Station Southbound
                try:
                    idx_time = update.find("time: ")
                    if idx_time != -1:
                        sub_part = update[idx_time + 6:]
                        end_idx = 0
                        while end_idx < len(sub_part) and sub_part[end_idx].isdigit():
                            end_idx += 1
                        val_str = sub_part[:end_idx]
                        if val_str:
                            arr_time = int(val_str)
                            diff_s = arr_time - now
                            diff_m = int(round(diff_s / 60.0))
                            if diff_m >= 0 and diff_m < 120:
                                arrivals.append(diff_m)
                except Exception as e:
                    print(f"Subway parse error: {e}", file=sys.stderr)
                    pass
                    
    return sorted(list(set(arrivals)))[:2]

def get_bus_predictions():
    url = 'https://retro.umoiq.com/service/publicXMLFeed?command=predictions&a=ttc&stopId=2268'
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        with urllib.request.urlopen(req, timeout=5.0) as r:
            xml_data = r.read()
        root = ET.fromstring(xml_data)
    except Exception as e:
        print(f"Bus fetch error: {e}", file=sys.stderr)
        return "No Bus Data"
    
    route_preds = {}
    for predictions in root.findall('.//predictions'):
        route_tag = predictions.get('routeTag')
        if route_tag not in ['34', '103', '74']:
            continue
        mins = []
        for prediction in predictions.findall('.//prediction'):
            mins.append(prediction.get('minutes'))
        if mins:
            route_preds[route_tag] = mins[:2]
            
    parts = []
    if '34' in route_preds:
        m_str = ",".join([f"{m}m" for m in route_preds['34']])
        parts.append(f"34:{m_str}")
        
    other = None
    if '103' in route_preds:
        other = '103'
    elif '74' in route_preds:
        other = '74'
        
    if other:
        m_str = ",".join([f"{m}m" for m in route_preds[other]])
        parts.append(f"{other}:{m_str}")
        
    if not parts:
        return "No Dunfield Bus"
        
    return " | ".join(parts)[:23]

def run_cmd(args):
    try:
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.SubprocessError as e:
        print(f"Failed to run command {args}: {e}", file=sys.stderr)

def main():
    print("TTC Transit Live Board Daemon started.")
    while True:
        sub_arr = get_subway_predictions()
        bus_str = get_bus_predictions()
        
        sub1 = sub_arr[0] if len(sub_arr) > 0 else 0xFF
        sub2 = sub_arr[1] if len(sub_arr) > 1 else 0xFF
        
        now_str = datetime.datetime.now().strftime("%H:%M:%S")
        alert_str = f"Live TTC Feed @ {now_str}"
        
        print(f"[{now_str}] Subway: {sub_arr} | Bus: {bus_str}")
        cmd = [
            "python3", SCREEN_CTL, "-p", PORT, "transit",
            str(sub1), str(sub2), bus_str, alert_str
        ]
        subprocess.run(cmd)
        
        if sub1 == 0xFF:
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "mode", "solid"])
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "rgb", "100", "0", "0"])
        elif sub1 <= 3:
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "mode", "pulse"])
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "rgb", "150", "80", "0"])
        elif sub1 <= 7:
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "mode", "solid"])
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "rgb", "0", "120", "0"])
        else:
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "mode", "solid"])
            run_cmd(["python3", SCREEN_CTL, "-p", PORT, "rgb", "0", "30", "150"])
            
        time.sleep(45)

if __name__ == '__main__':
    main()
