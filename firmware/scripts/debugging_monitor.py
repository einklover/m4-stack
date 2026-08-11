import sys
import argparse
import re
import threading
import subprocess
from pathlib import Path
from datetime import datetime
from collections import deque
import time

# Try to import potentially missing packages
try:
    import serial
    from colorama import init, Fore, Style
    import matplotlib.pyplot as plt
    import matplotlib.animation as animation
except ImportError as e:
    missing_package = e.name
    print("\n" + "!" * 50)
    print(f" Error: The required package '{missing_package}' is not installed.")
    print("!" * 50)

    print(f"\nTo fix this, please run the following command in your terminal:\n")

    install_cmd = "pip install "
    packages = []
    if 'serial' in str(e): packages.append("pyserial")
    if 'colorama' in str(e): packages.append("colorama")
    if 'matplotlib' in str(e): packages.append("matplotlib")

    print(f"    {install_cmd}{' '.join(packages)}")

    print("\nExiting...")
    sys.exit(1)

# --- Global Variables for Data Sharing ---
# Store last 50 data points
MAX_POINTS = 50
time_data = deque(maxlen=MAX_POINTS)
free_mem_data = deque(maxlen=MAX_POINTS)
total_mem_data = deque(maxlen=MAX_POINTS)
data_lock = threading.Lock() # Prevent reading while writing

# Initialize colors
init(autoreset=True)

def get_color_for_line(line):
    """
    Classify log lines by type and assign appropriate colors.
    """
    line_upper = line.upper()

    if any(keyword in line_upper for keyword in ["ERROR", "[ERR]", "[SCT]", "FAILED", "WARNING"]):
        return Fore.RED
    if "[MEM]" in line_upper or "FREE:" in line_upper:
        return Fore.CYAN
    if any(keyword in line_upper for keyword in ["[GFX]", "[ERS]", "DISPLAY", "RAM WRITE", "RAM COMPLETE", "REFRESH", "POWERING ON", "FRAME BUFFER", "LUT"]):
        return Fore.MAGENTA
    if any(keyword in line_upper for keyword in ["[EBP]", "[BMC]", "[ZIP]", "[PARSER]", "[EHP]", "LOADING EPUB", "CACHE", "DECOMPRESSED", "PARSING"]):
        return Fore.GREEN
    if "[ACT]" in line_upper or "ENTERING ACTIVITY" in line_upper or "EXITING ACTIVITY" in line_upper:
        return Fore.YELLOW
    if any(keyword in line_upper for keyword in ["RENDERED PAGE", "[LOOP]", "DURATION", "WAIT COMPLETE"]):
        return Fore.BLUE
    if any(keyword in line_upper for keyword in ["[CPS]", "SETTINGS", "[CLEAR_CACHE]"]):
        return Fore.LIGHTYELLOW_EX
    if any(keyword in line_upper for keyword in ["ESP-ROM", "BUILD:", "RST:", "BOOT:", "SPIWP:", "MODE:", "LOAD:", "ENTRY", "[SD]", "STARTING CROSSPOINT", "VERSION"]):
        return Fore.LIGHTBLACK_EX
    if "[RBS]" in line_upper:
        return Fore.LIGHTCYAN_EX
    if "[KRS]" in line_upper:
        return Fore.LIGHTMAGENTA_EX
    if any(keyword in line_upper for keyword in ["EINKDISPLAY:", "STATIC FRAME", "INITIALIZING", "SPI INITIALIZED", "GPIO PINS", "RESETTING", "SSD1677", "E-INK"]):
        return Fore.LIGHTMAGENTA_EX
    if any(keyword in line_upper for keyword in ["[FNS]", "FOOTNOTE"]):
        return Fore.LIGHTGREEN_EX
    if any(keyword in line_upper for keyword in ["[CHAP]", "[OPDS]", "[COF]"]):
        return Fore.LIGHTYELLOW_EX

    return Fore.WHITE

def parse_memory_line(line):
    """
    Extracts Free and Total bytes from the specific log line.
    Format: [MEM] Free: 196344 bytes, Total: 226412 bytes, Min Free: 112620 bytes
    """
    # Regex to find 'Free: <digits>' and 'Total: <digits>'
    match = re.search(r"Free:\s*(\d+).*Total:\s*(\d+)", line)
    if match:
        try:
            free_bytes = int(match.group(1))
            total_bytes = int(match.group(2))
            return free_bytes, total_bytes
        except ValueError:
            return None, None
    return None, None

def serial_worker(port, baud):
    """
    Runs in a background thread. Reads the device log stream through the
    m4adb daemon socket (never opens the USB port directly), prints to
    console, and updates the data lists.
    """
    m4adb_py = Path(__file__).resolve().parent / "m4adb.py"
    print(f"{Fore.CYAN}--- Reading log via m4adb daemon (port={port}) ---{Style.RESET_ALL}")

    cmd = [sys.executable, str(m4adb_py)]
    if port:
        cmd += ["--port", port]
    cmd += ["logs"]
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace"
    )

    try:
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue

            # m4adb logs prints "HH:MM:SS <raw line>"; reuse the raw line
            raw_line = re.sub(r"^\d{2}:\d{2}:\d{2} ", "", line)

            # Add PC timestamp
            pc_time = datetime.now().strftime("%H:%M:%S")
            formatted_line = re.sub(r"^\[\d+\]", f"[{pc_time}]", raw_line)

            # Check for Memory Line
            if "[MEM]" in formatted_line:
                free_val, total_val = parse_memory_line(formatted_line)
                if free_val is not None:
                    with data_lock:
                        time_data.append(pc_time)
                        free_mem_data.append(free_val / 1024) # Convert to KB
                        total_mem_data.append(total_val / 1024) # Convert to KB

            # Print to console
            line_color = get_color_for_line(formatted_line)
            print(f"{line_color}{formatted_line}")

    except (OSError, BrokenPipeError):
        print(f"{Fore.RED}Device disconnected.{Style.RESET_ALL}")
    finally:
        if proc.poll() is None:
            proc.terminate()

def update_graph(frame):
    """
    Called by Matplotlib animation to redraw the chart.
    """
    with data_lock:
        if not time_data:
            return

        # Convert deques to lists for plotting
        x = list(time_data)
        y_free = list(free_mem_data)
        y_total = list(total_mem_data)

    plt.cla() # Clear axis

    # Plot Total RAM
    plt.plot(x, y_total, label='Total RAM (KB)', color='red', linestyle='--')

    # Plot Free RAM
    plt.plot(x, y_free, label='Free RAM (KB)', color='green', marker='o')

    # Fill area under Free RAM
    plt.fill_between(x, y_free, color='green', alpha=0.1)

    plt.title("ESP32 Memory Monitor")
    plt.ylabel("Memory (KB)")
    plt.xlabel("Time")
    plt.legend(loc='upper left')
    plt.grid(True, linestyle=':', alpha=0.6)

    # Rotate date labels
    plt.xticks(rotation=45, ha='right')
    plt.tight_layout()

def main():
    parser = argparse.ArgumentParser(description="ESP32 Monitor with Graph")
    parser.add_argument("port", nargs="?", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    args = parser.parse_args()

    # 1. Start the Serial Reader in a separate thread
    # Daemon=True means this thread dies when the main program closes
    t = threading.Thread(target=serial_worker, args=(args.port, args.baud), daemon=True)
    t.start()

    # 2. Set up the Graph (Main Thread)
    try:
        plt.style.use('light_background')
    except:
        pass

    fig = plt.figure(figsize=(10, 6))

    # Update graph every 1000ms
    ani = animation.FuncAnimation(fig, update_graph, interval=1000)

    try:
        print(f"{Fore.YELLOW}Starting Graph Window... (Close window to exit){Style.RESET_ALL}")
        plt.show()
    except KeyboardInterrupt:
        print(f"\n{Fore.YELLOW}Exiting...{Style.RESET_ALL}")
        plt.close('all') # Force close any lingering plot windows

if __name__ == "__main__":
    main()
