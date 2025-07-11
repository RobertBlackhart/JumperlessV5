#include "FilesystemStuff.h"
#include "Graphics.h"
#include "oled.h"
#include <time.h>
#include "RotaryEncoder.h"
#include "config.h"
#include <cstring>
#include "Menus.h"


// External references
extern class oled oled;

// eKilo editor integration
#include "EkiloEditor.h"

// Global flag to signal return to main menu after editing
static bool returnToMainMenu = false;

FileManager::FileManager() {
    currentPath = "/";
    maxFiles = 100;
    fileList = new FileEntry[maxFiles];
    fileCount = 0;
    selectedIndex = 0;
    displayOffset = 0;
    maxDisplayLines = 80; // Adjust based on terminal size
    
    // Initialize OLED batching
    lastInputTime = 0;
    oledUpdatePending = false;
    
    // Initialize encoder state tracking
    lastEncoderDirectionState = NONE;
    lastEncoderButtonState = IDLE;
    
    // Initialize OLED scrolling
    oledHorizontalOffset = 0;
    oledCursorPosition = 0;
    
    // Initialize REPL mode
    replMode = false;
    shouldExitForREPL = false;
    originalCursorRow = 0;
    originalCursorCol = 0;
    startRow = 0;
    linesUsed = 0;
    lastOpenedFileContent = "";
    
    // Initialize filesystem
    initializeFilesystem();
}

FileManager::~FileManager() {
    delete[] fileList;
}

void FileManager::initializeFilesystem() {
    //Serial.println("[FS] Initializing filesystem...");
    
    // Try to initialize FatFS (it may already be initialized by main firmware)
    bool fs_ok = true;
    
    // Test if we can access the filesystem by checking basic operations
    if (!FatFS.exists("/")) {
        //Serial.println("[FS] Root directory not accessible, trying to initialize FatFS...");
        
        // Try to begin/mount the filesystem
        if (!FatFS.begin()) {
            //Serial.println("[FS] Failed to mount FatFS filesystem");
            fs_ok = false;
        } else {
            //Serial.println("[FS] FatFS mounted successfully");
        }
    } else {
        //Serial.println("[FS] Root directory accessible");
    }
    
    if (fs_ok) {
        // Don't create fake directories - only work with what actually exists
        //Serial.println("[FS] Using existing filesystem structure");
        
        // Test root directory access one more time using correct Dir API
        Dir testRoot = FatFS.openDir("/");
        bool root_accessible = false;
        int test_count = 0;
        while (testRoot.next() && test_count < 3) {
            test_count++;
            root_accessible = true;
        }
        
        if (root_accessible) {
            //Serial.println("[FS] Filesystem initialization successful - root directory accessible");
        } else {
            //Serial.println("[FS] Warning: Root directory not accessible via Dir API");
            //Serial.println("[FS] Will show filesystem unavailable message");
            currentPath = "[NO_FS]";
        }
    } else {
        //Serial.println("[FS] Filesystem not available - file operations will be limited");
        currentPath = "[NO_FS]";
    }
}

FileType FileManager::getFileType(const String& filename) {
    String lower = filename;
    lower.toLowerCase();
    
    if (lower.endsWith(".py") || lower.endsWith(".pyw") || lower.endsWith(".pyi")) {
        return FILE_TYPE_PYTHON;

    } else if (lower.endsWith(".json")) {
        return FILE_TYPE_JSON;
    } else if (lower.endsWith(".cfg") || lower.endsWith(".conf") || lower.startsWith("config") || filename == "config.txt") {
        return FILE_TYPE_CONFIG;
    } else if (lower.startsWith("nodefileslot") && lower.endsWith(".txt")) {
        return FILE_TYPE_NODEFILES;
    } else if (lower.startsWith("netcolorsslot") && lower.endsWith(".txt")) {
        return FILE_TYPE_COLORS;
    } else if (lower.endsWith(".txt") || lower.endsWith(".md") || lower.endsWith(".readme")) {
        return FILE_TYPE_TEXT;
    }
    return FILE_TYPE_UNKNOWN;
}

String FileManager::getFileIcon(FileType type) {
    switch (type) {
        case FILE_TYPE_DIRECTORY: return "⌘";
        case FILE_TYPE_PYTHON: return "𓆚";
        case FILE_TYPE_TEXT: return "⍺";
        case FILE_TYPE_CONFIG: return "⚙";
        case FILE_TYPE_JSON: return "⟐";
        case FILE_TYPE_NODEFILES: return "☊";
        case FILE_TYPE_COLORS: return "⎃";
        default: return "⍺";
    }
}

String FileManager::formatFileSize(size_t size) {
    if (size < 1024) {
        return String(size) + " B";
    } else if (size < 1024 * 1024) {
        return String(size / 1024) + " KB";
    } else {
        return String(size / (1024 * 1024)) + " MB";
    }
}

String FileManager::formatDateTime(time_t timestamp) {
    if (timestamp == 0) {
        return "Unknown";
    }
    
    // Convert to local time structure
    struct tm* timeinfo = localtime(&timestamp);
    if (!timeinfo) {
        return String((long)timestamp);
    }
    
    // Format as MM/DD/YY HH:MM
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%m/%d/%y %H:%M", timeinfo);
    return String(buffer);
}

void FileManager::updateOLEDStatus() {
    if (!oled.oledConnected) return;
    
    // Get current selection info
    const char* selectedFileName = (fileCount > 0 && selectedIndex < fileCount) 
        ? fileList[selectedIndex].name.c_str() 
        : nullptr;
    
    if (!selectedFileName) {
        // No file selected, just show path
        oled.showFileStatus(currentPath.c_str(), fileCount, nullptr);
        return;
    }
    
    // Build full display text and calculate cursor position
    String pathPart = currentPath;
    String fullText;
    
    // Don't add extra "/" if currentPath is already root "/"
    // Add newline after path separator for better display formatting (except root)
    if (currentPath == "/") {
        fullText = pathPart + String(selectedFileName);
    } else {
        fullText = pathPart + "/\n" + String(selectedFileName);
    }
    
    // Cursor position is at the end of the selected filename
    oledCursorPosition = fullText.length();
    
    // Show the full text with cursor indicator (no horizontal scrolling needed)
    oled.showFileStatusScrolled(fullText.c_str(), fileCount, oledCursorPosition);
}

void FileManager::scheduleOLEDUpdate() {
    unsigned long currentTime = millis();
    
    // If no recent input, update immediately for responsiveness
    if (!oledUpdatePending || (currentTime - lastInputTime) > 200) {
        updateOLEDStatus();
        lastInputTime = currentTime;
        oledUpdatePending = false;
    } else {
        // Batch subsequent updates
        lastInputTime = currentTime;
        oledUpdatePending = true;
    }
}

void FileManager::processOLEDUpdate() {
    // Only update OLED if enough time has passed since last input
    if (oledUpdatePending && (millis() - lastInputTime) >= 50) { // 50ms delay
        updateOLEDStatus();
        oledUpdatePending = false;
    }
}

void FileManager::calculateHorizontalScrolling(const String& fullText, int cursorPos) {
    if (!oled.isConnected()) return;
    
    // Better estimate: small font can fit ~25-26 characters for 128px width
    const int maxVisibleChars = 25;
    
    // Calculate visible cursor position relative to current offset
    int visibleCursorPos = cursorPos - oledHorizontalOffset;
    
    // Scroll right if cursor is too close to right edge
    if (visibleCursorPos >= maxVisibleChars - OLED_SCROLL_MARGIN) {
        oledHorizontalOffset = cursorPos - maxVisibleChars + OLED_SCROLL_MARGIN + 1;
    }
    // Scroll left if cursor is too close to left edge  
    else if (visibleCursorPos < OLED_SCROLL_MARGIN) {
        oledHorizontalOffset = cursorPos - OLED_SCROLL_MARGIN;
    }
    
    // Keep offset within bounds
    if (oledHorizontalOffset < 0) {
        oledHorizontalOffset = 0;
    }
    if (oledHorizontalOffset > fullText.length() - maxVisibleChars) {
        oledHorizontalOffset = max(0, (int)fullText.length() - maxVisibleChars);
    }
}

// Calculate directory depth for visual indentation
int FileManager::calculatePathDepth(const String& path) {
    if (path == "/") return 0;
    
    int depth = 0;
    for (int i = 0; i < path.length(); i++) {
        if (path[i] == '/') depth++;
    }
    return depth - 1; // Subtract 1 because root "/" counts as depth 0
}

bool FileManager::changeDirectory(const String& path) {
    String newPath = path;
    
    // Handle relative paths
    if (!newPath.startsWith("/")) {
        if (currentPath.endsWith("/")) {
            newPath = currentPath + newPath;
        } else {
            newPath = currentPath + "/" + newPath;
        }
    }
    
    // Clean up path (remove double slashes, etc.)
    newPath.replace("//", "/");
    
    // Check if directory exists - handle root directory specially
    bool dirExists = false;
    if (newPath == "/") {
        // For root directory, use openDir instead of exists
        Dir testRoot = FatFS.openDir("/");
        int test_count = 0;
        while (testRoot.next() && test_count < 3) {
            test_count++;
            dirExists = true;
            break;
        }
    } else {
        dirExists = FatFS.exists(newPath.c_str());
    }
    
    if (dirExists) {
        currentPath = newPath;
        selectedIndex = 0;
        displayOffset = 0;
        refreshListing();
        scheduleOLEDUpdate();
        return true;
    }
    
    changeTerminalColor(FileColors::ERROR, false);
    Serial.println("Directory not found: " + newPath);
    changeTerminalColor(-1, false); // Reset colors
    return false;
}

bool FileManager::goUp() {
    if (currentPath == "/") return false;
    
    int lastSlash = currentPath.lastIndexOf('/');
    if (lastSlash == 0) {
        return changeDirectory("/");
    } else {
        return changeDirectory(currentPath.substring(0, lastSlash));
    }
}

bool FileManager::goHome() {
    return changeDirectory("/");
}

void FileManager::refreshListing() {
    static int recursion_depth = 0;
    
    // Prevent infinite recursion
    if (recursion_depth > 3) {
        Serial.println("[FS] Too many recursion attempts, marking filesystem as unavailable");
        currentPath = "[NO_FS]";
        recursion_depth = 0;
        // Fall through to handle [NO_FS] case
    }
    
    fileCount = 0;
    
    // Handle special case where filesystem is not available
    if (currentPath == "[NO_FS]") {
        recursion_depth = 0; // Reset counter
        // Create virtual entries to show filesystem unavailable message
        if (maxFiles > 0) {
            fileList[0].name = "** FILESYSTEM NOT AVAILABLE **";
            fileList[0].path = "[ERROR]";
            fileList[0].isDirectory = false;
            fileList[0].size = 0;
            fileList[0].type = FILE_TYPE_UNKNOWN;
            fileCount = 1;
            
            if (maxFiles > 1) {
                fileList[1].name = "Directory access failed after mount";
                fileList[1].path = "[INFO]";
                fileList[1].isDirectory = false;
                fileList[1].size = 0;
                fileList[1].type = FILE_TYPE_UNKNOWN;
                fileCount = 2;
                
                if (maxFiles > 2) {
                    fileList[2].name = "Press 'q' to quit file manager";
                    fileList[2].path = "[HELP]";
                    fileList[2].isDirectory = false;
                    fileList[2].size = 0;
                    fileList[2].type = FILE_TYPE_UNKNOWN;
                    fileCount = 3;
                }
            }
        }
        return;
    }
    
    // Use FatFS directory API instead of file API
    Dir dir = FatFS.openDir(currentPath);
    
    // Check if directory exists by testing if we can read from it
    bool dir_accessible = false;
    int test_entries = 0;
    
    // Test directory accessibility by trying to read a few entries
    while (dir.next() && test_entries < 5) {
        test_entries++;
        dir_accessible = true;
    }
    
    if (!dir_accessible) {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Failed to open directory: " + currentPath);
        changeTerminalColor(-1, false); // Reset colors
        
        recursion_depth++;
        
        // Try to recover by going to root or alternative directory (only if not too deep)
        if (recursion_depth <= 2 && currentPath != "/") {
            Serial.println("[FS] Attempting to return to root directory...");
            currentPath = "/";
            Dir testRoot = FatFS.openDir("/");
            int test_count = 0;
            bool root_ok = false;
            while (testRoot.next() && test_count < 3) {
                test_count++;
                root_ok = true;
            }
            if (root_ok) {
                refreshListing(); // Recursive call to try root
                recursion_depth--; // Decrement on successful path
                return;
            }
            
            // No alternative directories to try - filesystem issues
        }
        
        // If all fails, mark filesystem as unavailable
        Serial.println("[FS] All recovery attempts failed, filesystem unavailable");
        currentPath = "[NO_FS]";
        recursion_depth = 0; // Reset for next attempt
        refreshListing(); // Show error message
        return;
    }
    
    // Successfully opened directory, reset recursion counter
    recursion_depth = 0;
    
    // Add ".." entry if not at root
    if (currentPath != "/") {
        FileEntry& parentEntry = fileList[fileCount];
        parentEntry.name = "..";
        parentEntry.path = "[UP]"; // Special marker for parent directory
        parentEntry.isDirectory = true;
        parentEntry.size = 0;
        parentEntry.lastModified = 0;
        parentEntry.type = FILE_TYPE_DIRECTORY;
        fileCount++;
    }
    
    // Re-open directory to start from beginning for actual listing
    dir = FatFS.openDir(currentPath);
    
    while (dir.next() && fileCount < maxFiles) {
        String fileName = dir.fileName();
        
        // Skip hidden files (files starting with '.') except for ".." navigation
        if (fileName.startsWith(".") && fileName != "..") {
            continue;
        }
        
        FileEntry& entry = fileList[fileCount];
        entry.name = fileName;
        entry.path = getFullPath(currentPath, entry.name);
        entry.isDirectory = dir.isDirectory();
        entry.size = dir.isDirectory() ? 0 : dir.fileSize();
        entry.lastModified = dir.fileCreationTime(); // Use creation time
        
        if (entry.isDirectory) {
            entry.type = FILE_TYPE_DIRECTORY;
        } else {
            entry.type = getFileType(entry.name);
        }
        
        fileCount++;
    }
    
    // Sort directories first, then files alphabetically
    // But keep ".." at the very top if it exists
    int startSort = 0;
    if (fileCount > 0 && fileList[0].name == "..") {
        startSort = 1; // Skip the ".." entry in sorting
    }
    
    for (int i = startSort; i < fileCount - 1; i++) {
        for (int j = i + 1; j < fileCount; j++) {
            bool shouldSwap = false;
            
            if (fileList[i].isDirectory && !fileList[j].isDirectory) {
                shouldSwap = false; // Keep directory first
            } else if (!fileList[i].isDirectory && fileList[j].isDirectory) {
                shouldSwap = true; // Move directory up
            } else {
                // Same type, sort alphabetically
                shouldSwap = fileList[i].name > fileList[j].name;
            }
            
            if (shouldSwap) {
                FileEntry temp = fileList[i];
                fileList[i] = fileList[j];
                fileList[j] = temp;
            }
        }
    }
}

void FileManager::showCurrentListing(bool showHeader) {
    if (showHeader) {
        changeTerminalColor(FileColors::HEADER, true);
        Serial.println("\n╭───────────────────────────────────────────────────────────────────────────╮");
        Serial.println("│                            JUMPERLESS FILE MANAGER                        │");
        Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
        
        changeTerminalColor(FileColors::STATUS, false);
        Serial.print("⌘ Current Path: ");
        printColoredPath(currentPath);
        Serial.println();
        
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("Files: " + String(fileCount) + "  |  Use ↑↓ arrows or rotary encoder to navigate");
        Serial.println("Enter/Click: Open/Edit  |  Space: File operations  |  h: Help  |  q: Quit");
        Serial.println("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌");
    }
    
    // Calculate display range
    int startIdx = displayOffset;
    int endIdx = min(displayOffset + maxDisplayLines, fileCount);
    
    for (int i = startIdx; i < endIdx; i++) {
        bool isSelected = (i == selectedIndex);
        FileEntry& entry = fileList[i];
        
        // Position cursor for this file entry
        moveCursor(6 + (i - startIdx), 3);
        clearCurrentLine();
        
        // Calculate indentation based on directory depth
        int currentDepth = calculatePathDepth(currentPath);
        String indentPrefix = "";
        for (int d = 0; d < currentDepth; d++) {
            indentPrefix += "  "; // 2 spaces per level
        }
        
        // Selection indicator
        if (isSelected) {
            changeTerminalColor(226, false); // Bright yellow background
            Serial.print("► ");
        } else {
            Serial.print("  ");
        }
        
        // Add visual indentation
        Serial.print(indentPrefix);
        
        // File type color and icon
        int color = FileColors::UNKNOWN;
        switch (entry.type) {
            case FILE_TYPE_DIRECTORY: color = FileColors::DIRECTORY; break;
            case FILE_TYPE_PYTHON: color = FileColors::PYTHON; break;
            case FILE_TYPE_TEXT: color = FileColors::TEXT; break;
            case FILE_TYPE_CONFIG: color = FileColors::CONFIG; break;
            case FILE_TYPE_JSON: color = FileColors::JSON; break;
            case FILE_TYPE_NODEFILES: color = FileColors::NODEFILES; break;
            case FILE_TYPE_COLORS: color = FileColors::COLORS; break;

        }
        
        changeTerminalColor(color, false);
        
        // Special handling for ".." entry
        if (entry.name == ".." && entry.path == "[UP]") {
            Serial.print("⌘ ");
            Serial.print("..");
            
            // Add padding to align with size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length() + 2 + 2; // selector + indent + icon + ".."
            int padding = 50 - usedSpace;
            for (int p = 0; p < padding && p >= 0; p++) Serial.print(" ");
        } else {
            Serial.print(getFileIcon(entry.type) + " ");
            
            // Filename
            String displayName = entry.name;
            int maxNameLength = 45 - (currentDepth * 2); // Adjust for indentation
            if (displayName.length() > maxNameLength) {
                displayName = displayName.substring(0, maxNameLength - 3) + "...";
            }
            Serial.print(displayName);
            
            // Padding for size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length() + 2 + displayName.length(); // selector + indent + icon + name
            int padding = 50 - usedSpace;
            for (int p = 0; p < padding && p >= 0; p++) Serial.print(" ");
        }
        
        changeTerminalColor(248, false); // Light grey for size
        if (entry.name == ".." && entry.path == "[UP]") {
            Serial.print("     <UP>");
        } else if (entry.isDirectory) {
            Serial.print("    <DIR>");
        } else {
            String sizeStr = formatFileSize(entry.size);
            int sizeWidth = 10 - sizeStr.length();
            for (int s = 0; s < sizeWidth && s >= 0; s++) Serial.print(" ");
            Serial.print(sizeStr);
        }
        
        Serial.println();
    }
    
    // Show scroll indicator if needed
    if (fileCount > maxDisplayLines) {
        changeTerminalColor(FileColors::STATUS, true);
        Serial.println("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌");
        Serial.println("Showing " + String(startIdx + 1) + "-" + String(endIdx) + " of " + String(fileCount) + " files");
    }
    
    changeTerminalColor(-1, false); // Reset colors
}

void FileManager::showFileInfo(const FileEntry& file) {
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                                FILE INFO                                  │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    
    changeTerminalColor(FileColors::STATUS, false);
    Serial.println("Name: " + file.name);
    Serial.println("Path: " + file.path);
    Serial.print("Type: ");
    Serial.println(file.isDirectory ? "Directory" : "File");
    if (!file.isDirectory) {
        Serial.println("Size: " + formatFileSize(file.size));
    }
    if (file.lastModified != 0) {
        Serial.println("Modified: " + formatDateTime(file.lastModified));
    }
    Serial.println("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌");
    
    changeTerminalColor(-1, false); // Reset colors
    Serial.println("\nPress any key to continue...");
}

void FileManager::showHelp() {
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                          FILE MANAGER HELP                                │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("\n⌘ NAVIGATION:");
    changeTerminalColor(221, false); // Yellow
    Serial.print("  ↑/↓ or encoder  ");
    changeTerminalColor(248, false);
    Serial.println("- Move selection up/down");
    changeTerminalColor(221, false);
    Serial.print("  Enter           ");
    changeTerminalColor(248, false);
    Serial.println("- Open directory or edit file");
    changeTerminalColor(221, false);
    Serial.print("  ..              ");
    changeTerminalColor(248, false);
    Serial.println("- Go up one directory");
    changeTerminalColor(221, false);
    Serial.print("  /               ");
    changeTerminalColor(248, false);
    Serial.println("- Go to root directory");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("\n⍺ FILE OPERATIONS:");
    changeTerminalColor(155, false); // Green
    Serial.print("  v               ");
    changeTerminalColor(248, false);
    Serial.println("- View file contents");
    changeTerminalColor(155, false);
    Serial.print("  e               ");
    changeTerminalColor(248, false);
    Serial.println("- Edit with eKilo editor");
    changeTerminalColor(155, false);
    Serial.print("  n               ");
    changeTerminalColor(248, false);
    Serial.println("- Create new file");
    changeTerminalColor(155, false);
    Serial.print("  d               ");
    changeTerminalColor(248, false);
    Serial.println("- Create new directory");
    changeTerminalColor(155, false);
    Serial.print("  r               ");
    changeTerminalColor(248, false);
    Serial.println("- Rename file/directory");
    changeTerminalColor(155, false);
    Serial.print("  x               ");
    changeTerminalColor(248, false);
    Serial.println("- Delete file/directory");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("\n⟐ OTHER:");
    changeTerminalColor(207, false); // Magenta
    Serial.print("  i               ");
    changeTerminalColor(248, false);
    Serial.println("- Show file info");
    changeTerminalColor(207, false);
    Serial.print("  h               ");
    changeTerminalColor(248, false);
    Serial.println("- Show this help");
    changeTerminalColor(207, false);
    Serial.print("  q               ");
    changeTerminalColor(248, false);
    Serial.println("- Quit file manager");
    changeTerminalColor(207, false);
    Serial.print("  t               ");
    changeTerminalColor(248, false);
    Serial.println("- Format/indent Python files in directory");
    
    changeTerminalColor(FileColors::ERROR, true);
    Serial.println("\n○ FILE TYPE COLORS:");
    changeTerminalColor(FileColors::DIRECTORY, false);
    Serial.print("⌘ Directories  ");
    changeTerminalColor(FileColors::PYTHON, false);
    Serial.print("𓆚 Python  ");
    changeTerminalColor(FileColors::TEXT, false);
    Serial.print("⍺ Text  ");
    changeTerminalColor(FileColors::CONFIG, false);
    Serial.print("⚙ Config  ");
    changeTerminalColor(FileColors::NODEFILES, false);
    Serial.print("☊ NodeFiles  ");
    changeTerminalColor(FileColors::COLORS, false);
    Serial.println("⎃ Colors");
    
    changeTerminalColor(-1, false); // Reset colors
    Serial.println("\nPress any key to continue...");
}

void FileManager::moveSelection(int direction) {
    if (fileCount == 0) return;
    
    selectedIndex += direction;
    
    // Wrap around
    if (selectedIndex < 0) {
        selectedIndex = fileCount - 1;
    } else if (selectedIndex >= fileCount) {
        selectedIndex = 0;
    }
    
    // Reset horizontal scrolling when selection changes
    oledHorizontalOffset = 0;
    oledCursorPosition = 0;
    
    // Adjust display offset if needed (limit to 15 visible lines)
    int maxVisible = 15;
    if (selectedIndex < displayOffset) {
        displayOffset = selectedIndex;
    } else if (selectedIndex >= displayOffset + maxVisible) {
        displayOffset = selectedIndex - maxVisible + 1;
    }
    
    // Update display in place
    updateStatusLine();
    updateFileListDisplay();
    scheduleOLEDUpdate();
}

FileEntry* FileManager::getCurrentFile() {
    if (selectedIndex >= 0 && selectedIndex < fileCount) {
        return &fileList[selectedIndex];
    }
    return nullptr;
}

void FileManager::selectCurrentFile() {
    FileEntry* file = getCurrentFile();
    if (!file) return;
    
    // Handle special ".." entry
    if (file->name == ".." && file->path == "[UP]") {
        if (goUp()) {
            // Successfully went up - redraw the interface
            drawInterface();
        }
        return;
    }
    
    if (file->isDirectory) {
        if (changeDirectory(file->path)) {
            // Successfully changed directory - redraw the interface
            drawInterface();
        }
    } else {
        editFile(file->path);
        refreshListing(); // Refresh in case file was modified
        drawInterface(); // Redraw entire interface after editing
    }
}

bool FileManager::createFile(const String& filename) {
    String fullPath = getFullPath(currentPath, filename);
    
    if (FatFS.exists(fullPath.c_str())) {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("File already exists: " + filename);
        changeTerminalColor(-1, false); // Reset colors
        return false;
    }
    
    File file = FatFS.open(fullPath.c_str(), "w");
    if (file) {
        file.close();
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("Created file: " + filename);
        changeTerminalColor(-1, false); // Reset colors
        refreshListing();
        return true;
    } else {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Failed to create file: " + filename);
        changeTerminalColor(-1, false); // Reset colors
        return false;
    }
}

bool FileManager::deleteFile(const String& filename) {
    String fullPath = getFullPath(currentPath, filename);
    
    if (FatFS.remove(fullPath.c_str())) {
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("Deleted: " + filename);
        changeTerminalColor(-1, false);
        refreshListing();
        return true;
    } else {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Failed to delete: " + filename);
        changeTerminalColor(-1, false);
        return false;
    }
}

bool FileManager::editFile(const String& filename) {
    changeTerminalColor(FileColors::STATUS, false);
    Serial.println("\n\n\rOpening " + filename + " in text editor...");
    changeTerminalColor(-1, false); // Reset colors
    
    return editFileWithEkilo(filename);
}

bool FileManager::editFileWithEkilo(const String& filename) {
    if (replMode) {
        // In REPL mode - use launchEkiloREPL and store returned content
        String content = launchEkiloREPL(filename.c_str());
        
        if (content.length() > 0) {
            // User saved new content
            lastOpenedFileContent = content;
            shouldExitForREPL = true; // Signal to exit file manager
        } else {
            // User didn't save - try to load existing file content if it exists
            File file = FatFS.open(filename.c_str(), "r");
            if (file) {
                String existingContent = file.readString();
                file.close();
                if (existingContent.length() > 0) {
                    lastOpenedFileContent = existingContent;
                    shouldExitForREPL = true; // Signal to exit file manager
                }
            }
        }
    } else {
        // Normal mode - use regular launchEkilo
        launchEkilo(filename.c_str());
    }
    return true;
}

bool FileManager::viewFile(const String& filename) {
    File file = FatFS.open(filename.c_str(), "r");
    if (!file) {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Failed to open file: " + filename);
        changeTerminalColor(-1, false); // Reset colors
        return false;
    }
    
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                              FILE VIEWER                                  │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    Serial.println("File: " + filename);
    Serial.println("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌");
    
    changeTerminalColor(248, true); // Light grey for content
    
    int lineCount = 0;
    while (file.available() && lineCount < 50) { // Limit to 50 lines for viewing
        String line = file.readStringUntil('\n');
        Serial.println(line);
        lineCount++;
    }
    
    if (file.available()) {
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("\n... (file continues - use 'e' to edit full file)");
    }
    
    file.close();
    changeTerminalColor(-1, false); // Reset colors
    Serial.println("\nPress any key to continue...");
    
    while (Serial.available() == 0) {
        delayMicroseconds(100); // Use normal delay for blocking wait
    }
    while (Serial.available() > 0) {
        Serial.read();
    }
    
    return true;
}

void FileManager::run() {
    refreshListing();
    bool running = true;
    
    // Initialize interactive mode
    initInteractiveMode();
    
    // Initial display
    drawInterface();
    
    while (running) {
        // In REPL mode, check if we should exit after content is ready
        if (replMode && shouldExitForREPL) {
            running = false;
            break;
        }
        
        // Process any pending OLED updates
        processOLEDUpdate();
        
        // Note: Removed returnToMainMenu check - editor returns directly to file manager now
        
        // Handle rotary encoder input
       // rotaryEncoderStuff();
        
        // Check for encoder direction changes
        if (encoderDirectionState != lastEncoderDirectionState) {
            if (encoderDirectionState == UP) {
                encoderDirectionState = NONE;
                moveSelection(-1);
                scheduleOLEDUpdate();
                lastInputTime = micros(); // Record input time
            } else if (encoderDirectionState == DOWN) {
                encoderDirectionState = NONE;
                moveSelection(1);
                scheduleOLEDUpdate();
                lastInputTime = micros(); // Record input time
            }
            lastEncoderDirectionState = encoderDirectionState;
        }
        
        // Check for encoder button presses
        if (encoderButtonState != lastEncoderButtonState) {
            if (encoderButtonState == PRESSED && lastEncoderButtonState == IDLE) {
                selectCurrentFile();
                scheduleOLEDUpdate();
                lastInputTime = micros(); // Record input time
                // Note: selectCurrentFile() now handles interface redrawing internally
            }
            lastEncoderButtonState = encoderButtonState;
        }
        
        // Wait for input without blocking
        char input = 0;
        if (Serial.available()) {
            input = Serial.read();
            lastInputTime = micros(); // Record input time for any serial input
            scheduleOLEDUpdate(); // Schedule OLED update for any input
        } else {
            delayMicroseconds(100);
            continue;
        }
        
        // Handle input
        switch (input) {
            case 'q':
            case 'Q':
            case 17: // Ctrl-Q
                running = false;
                break;
                
            case '\r':
            case '\n':
                selectCurrentFile();
                // Note: selectCurrentFile() now handles interface redrawing internally
                break;
                
            case 'h':
            case 'H':
                showInteractiveHelp();
                break;
                
            case 'v':
            case 'V': {
                FileEntry* file = getCurrentFile();
                if (file && !file->isDirectory) {
                    showInteractiveFileView(file->path);
                }
                break;
            }
            
            case 'e':
            case 'E': {
                FileEntry* file = getCurrentFile();
                if (file && !file->isDirectory) {
                    editFile(file->path);
                    refreshListing(); // Refresh in case file was modified
                    drawInterface(); // Redraw entire interface
                }
                break;
            }
            
            case 'i':
            case 'I': {
                FileEntry* file = getCurrentFile();
                if (file) {
                    showInteractiveFileInfo(*file);
                }
                break;
            }
            
            case 'n':
            case 'N': {
                String filename = promptForFilename("Enter filename (with extension): ");
                if (filename.length() > 0 && isValidFilename(filename)) {
                    createFile(filename);
                    refreshListing();
                    drawInterface();
                }
                break;
            }
            
            case 'd':
            case 'D': {
                String dirname = promptForFilename("Enter directory name: ");
                if (dirname.length() > 0 && isValidFilename(dirname)) {
                    createDirectory(dirname);
                    refreshListing();
                    drawInterface();
                }
                break;
            }
            
            case 'x':
            case 'X': {
                FileEntry* file = getCurrentFile();
                if (file && !(file->name == ".." && file->path == "[UP]")) {
                    changeTerminalColor(FileColors::ERROR, false);
                    if (file->isDirectory) {
                        Serial.println("\n\rDelete directory '" + file->name + "'? (y/N): ");
                    } else {
                        Serial.println("\n\rDelete file '" + file->name + "'? (y/N): ");
                    }
                    Serial.flush();
                    changeTerminalColor(-1, false);
                    
                    while (Serial.available() == 0) delayMicroseconds(100);
                    char confirm = Serial.read();
                    Serial.println(confirm);
                    Serial.flush();
                    
                    if (confirm == 'y' || confirm == 'Y') {
                        deleteFile(file->name);
                        Serial.println("Deleting...");
                        Serial.flush();
                        refreshListing();
                        drawInterface();
                    }
                    
                }
                break;
            }
            
            case 27: { // Escape sequence (arrow keys)
                if (Serial.available()) {
                    char seq1 = Serial.read();
                    if (seq1 == '[' && Serial.available()) {
                        char seq2 = Serial.read();
                        switch (seq2) {
                            case 'A': moveSelection(-1); break; // Up arrow
                            case 'B': moveSelection(1); break;  // Down arrow
                        }
                    }
                }
                break;
            }
            
            case '/':
                if (goHome()) {
                    drawInterface();
                }
                break;
                
            case '.':
                if (currentPath != "/") {
                    if (goUp()) {
                        drawInterface();
                    }
                }
                break;
                
            case 'r':
            case 'R': {
                // Refresh directory listing
                refreshListing();
                updateStatusLine();
                updateFileListDisplay();
                break;
            }
            

        }
    }
    
    // Clean up interactive mode
    clearScreen();
    showCursor();
    
    // Restore normal font if we were using small fonts
    if (oled.oledConnected) {
        oled.restoreNormalFont();
    }
    
    // Exit Jumperless interactive mode
    Serial.write(0x0F);
    Serial.flush();
    delay(100); // Give system time to switch modes
    
    changeTerminalColor(FileColors::STATUS, false);
    Serial.println("Exiting File Manager...");
    changeTerminalColor(-1, false); // Reset colors
    
    // Print main menu when returning from editor
    //printMainMenu(0);
}

// Interactive mode implementation
void FileManager::initInteractiveMode() {
    // Enter Jumperless interactive mode
    Serial.write(0x0E);
    Serial.flush();
    delay(100); // Give system time to switch modes
    
    //clearScreen();
    //hideCursor();
}

void FileManager::clearScreen() {
    Serial.print("\033[2J");    // Clear entire screen
    Serial.print("\033[H");     // Move cursor to home (1,1)
    Serial.flush();
}

void FileManager::moveCursor(int row, int col) {
    Serial.print("\033[");
    Serial.print(row);
    Serial.print(";");
    Serial.print(col);
    Serial.print("H");
    Serial.flush();
}

void FileManager::hideCursor() {
    // Serial.print("\033[?25l");
    // Serial.flush();
}

void FileManager::showCursor() {
    // Serial.print("\033[?25h");
    // Serial.flush();
}

void FileManager::clearCurrentLine() {
    Serial.print("\033[2K");    // Clear entire line
    Serial.print("\r");         // Return to beginning of line
    Serial.flush();
}

void FileManager::drawInterface(bool fullScreen) {
    // Always use full interface - no special REPL mode anymore
    // Full screen mode (original behavior)
    // Position for header
    if (fullScreen) {
        moveCursor(1, 1);
    }
    // Draw header
    changeTerminalColor(FileColors::HEADER, false);
    Serial.println("╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                            JUMPERLESS FILE MANAGER                        │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    changeTerminalColor(-1, false); // Reset colors
    
    // Draw status line
    updateStatusLine();
    
    // Draw file listing
    updateFileListDisplay();
    
    // Draw comprehensive help lines at bottom (2 lines in magenta)
    if (fullScreen) {
        moveCursor(22, 1);
    }
    changeTerminalColor(125, false); 
    Serial.print(" enter = open   │ h = help │ q = quit │ ↑↓/wheel = nav │ . = up dir  │ / = root");
    if (fullScreen) {
        moveCursor(23, 1);
    }
    changeTerminalColor(89, false); 
    Serial.print(" v = quick view │ e = edit │ i = info │  n = new file  │ d = new dir │ x = delete");
    changeTerminalColor(-1, false); // Reset colors
    Serial.flush();
}

void FileManager::updateStatusLine() {
    moveCursor(4, 1);
    clearCurrentLine();
    
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("⌘ Current Path: ");
    changeTerminalColor(FileColors::DIRECTORY, false);
    
    // Truncate path if too long
    String displayPath = currentPath;
    if (displayPath.length() > 50) {
        displayPath = "..." + displayPath.substring(displayPath.length() - 47);
    }
    Serial.print(displayPath);
    
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("  |  Files: ");
    Serial.print(fileCount);
    if (fileCount > 0) {
        Serial.print("  |  Selected: ");
        Serial.print(selectedIndex + 1);
        Serial.print("/");
        Serial.print(fileCount);
    }
    
    Serial.print("\x1b[0m"); // Reset colors
    Serial.flush();
}

void FileManager::updateFileListDisplay() {
    // Always use full interface - no special REPL mode anymore
        // Full screen mode (original behavior)
        // Clear file listing area (lines 6-20)
        for (int i = 6; i <= 20; i++) {
            moveCursor(i, 1);
            clearCurrentLine();
        }
        
        // Handle special case where filesystem is not available
        if (currentPath == "[NO_FS]") {
            moveCursor(8, 5);
            changeTerminalColor(FileColors::ERROR, false);
            Serial.print("** FILESYSTEM NOT AVAILABLE **");
            
            moveCursor(10, 5);
            changeTerminalColor(FileColors::STATUS, false);
            Serial.print("Directory access failed after mount");
            
            moveCursor(12, 5);
            Serial.print("Press 'q' to quit file manager");
            
            changeTerminalColor(0, false);
            Serial.flush();
            delayMicroseconds(100); // Short delay to show error
            return;
        }
        
        // Calculate display range
        int startIdx = displayOffset;
        int endIdx = min(displayOffset + maxDisplayLines, fileCount);
        endIdx = min(endIdx, startIdx + 15); // Limit to 15 lines visible
        
        for (int i = startIdx; i < endIdx; i++) {
            bool isSelected = (i == selectedIndex);
            FileEntry& entry = fileList[i];
            
            // Position cursor for this file entry
            moveCursor(6 + (i - startIdx), 3);
            clearCurrentLine();
            
            // Calculate indentation based on directory depth
            int currentDepth = calculatePathDepth(currentPath);
            String indentPrefix = "";
            for (int d = 0; d < currentDepth; d++) {
                indentPrefix += "  "; // 2 spaces per level
            }
            
            // Selection indicator
            if (isSelected) {
                changeTerminalColor(226, false); // Bright yellow background
                Serial.print("► ");
            } else {
                Serial.print("  ");
            }
            
            // Add visual indentation
            Serial.print(indentPrefix);
            
            // File type color and icon
            int color = FileColors::UNKNOWN;
            switch (entry.type) {
                case FILE_TYPE_DIRECTORY: color = FileColors::DIRECTORY; break;
                case FILE_TYPE_PYTHON: color = FileColors::PYTHON; break;
                case FILE_TYPE_TEXT: color = FileColors::TEXT; break;
                case FILE_TYPE_CONFIG: color = FileColors::CONFIG; break;
                case FILE_TYPE_JSON: color = FileColors::JSON; break;
                case FILE_TYPE_NODEFILES: color = FileColors::NODEFILES; break;
                case FILE_TYPE_COLORS: color = FileColors::COLORS; break;
            }
            
            changeTerminalColor(color, false);
            
            // Special handling for ".." entry
            if (entry.name == ".." && entry.path == "[UP]") {
                Serial.print("⌘ ");
                Serial.print("..");
                
                // Add padding to align with size column (adjusted for indentation)
                int usedSpace = 2 + indentPrefix.length() + 2 + 2; // selector + indent + icon + ".."
                int padding = 50 - usedSpace;
                for (int p = 0; p < padding && p >= 0; p++) Serial.print(" ");
            } else {
                Serial.print(getFileIcon(entry.type) + " ");
                
                // Filename
                String displayName = entry.name;
                int maxNameLength = 45 - (currentDepth * 2); // Adjust for indentation
                if (displayName.length() > maxNameLength) {
                    displayName = displayName.substring(0, maxNameLength - 3) + "...";
                }
                Serial.print(displayName);
                
                // Padding for size column (adjusted for indentation)
                int usedSpace = 2 + indentPrefix.length() + 2 + displayName.length(); // selector + indent + icon + name
                int padding = 50 - usedSpace;
                for (int p = 0; p < padding && p >= 0; p++) Serial.print(" ");
            }
            
            changeTerminalColor(248, false); // Light grey for size
            if (entry.name == ".." && entry.path == "[UP]") {
                Serial.print("     <UP>");
            } else if (entry.isDirectory) {
                Serial.print("    <DIR>");
            } else {
                String sizeStr = formatFileSize(entry.size);
                int sizeWidth = 10 - sizeStr.length();
                for (int s = 0; s < sizeWidth && s >= 0; s++) Serial.print(" ");
                Serial.print(sizeStr);
            }
            
            changeTerminalColor(0, false);
        }
        
        // Show scroll indicator if needed
        if (fileCount > 15) {
            moveCursor(21, 3);
            changeTerminalColor(FileColors::STATUS, false);
            Serial.print("Showing ");
            Serial.print(startIdx + 1);
            Serial.print("-");
            Serial.print(min(endIdx, fileCount));
            Serial.print(" of ");
            Serial.print(fileCount);
            Serial.print(" files");
            changeTerminalColor(0, false);
        }
        
        Serial.flush();
}

void FileManager::showInteractiveHelp() {
    // Save current screen
    clearScreen();
    
    // Draw help screen
    moveCursor(1, 1);
    changeTerminalColor(FileColors::HEADER, false);
    Serial.println("╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                          FILE MANAGER HELP                                │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    
    moveCursor(4, 3);
    changeTerminalColor(FileColors::STATUS, false);
            Serial.println("⌘ NAVIGATION:");
    moveCursor(5, 5);
    changeTerminalColor(221, false); // Yellow
    Serial.print("↑/↓ or encoder  ");
    changeTerminalColor(248, false);
    Serial.println("- Move selection up/down");
    moveCursor(6, 5);
    changeTerminalColor(221, false);
    Serial.print("Enter/Click     ");
    changeTerminalColor(248, false);
    Serial.println("- Open directory or edit file");
    moveCursor(7, 5);
    changeTerminalColor(221, false);
    Serial.print("..              ");
    changeTerminalColor(248, false);
    Serial.println("- Go up one directory");
    moveCursor(8, 5);
    changeTerminalColor(221, false);
    Serial.print("/               ");
    changeTerminalColor(248, false);
    Serial.println("- Go to root directory");
    
    moveCursor(10, 3);
    changeTerminalColor(FileColors::STATUS, false);
            Serial.println("⍺ FILE OPERATIONS:");
    moveCursor(11, 5);
    changeTerminalColor(155, false); // Green
    Serial.print("v               ");
    changeTerminalColor(248, false);
    Serial.println("- View file contents");
    moveCursor(12, 5);
    changeTerminalColor(155, false);
    Serial.print("e               ");
    changeTerminalColor(248, false);
    Serial.println("- Edit with eKilo editor");
    moveCursor(13, 5);
    changeTerminalColor(155, false);
    Serial.print("i               ");
    changeTerminalColor(248, false);
    Serial.println("- Show file information");
    
    moveCursor(15, 3);
    changeTerminalColor(FileColors::STATUS, false);
    Serial.println("⚙ CREATE/DELETE:");
    moveCursor(16, 5);
    changeTerminalColor(155, false); // Green
    Serial.print("n               ");
    changeTerminalColor(248, false);
    Serial.println("- Create new file");
    moveCursor(17, 5);
    changeTerminalColor(155, false);
    Serial.print("d               ");
    changeTerminalColor(248, false);
    Serial.println("- Create new directory");
    moveCursor(18, 5);
    changeTerminalColor(207, false); // Magenta (red)
    Serial.print("x               ");
    changeTerminalColor(248, false);
    Serial.println("- Delete file/directory");
    
    moveCursor(20, 3);
    changeTerminalColor(FileColors::STATUS, false);
    Serial.println("⟐ OTHER:");
    moveCursor(21, 5);
    changeTerminalColor(207, false); // Magenta
    Serial.print("h               ");
    changeTerminalColor(248, false);
    Serial.println("- Show this help");
    moveCursor(22, 5);
    changeTerminalColor(207, false);
    Serial.print("q               ");
    changeTerminalColor(248, false);
    Serial.println("- Quit file manager");
    
    moveCursor(24, 3);
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("Press any key to return...");
    changeTerminalColor(0, false);
    Serial.flush();
    
    // Wait for keypress
    while (Serial.available() == 0) delayMicroseconds(100);
    while (Serial.available() > 0) Serial.read();
    
    // Redraw main interface
    drawInterface();
}

void FileManager::showInteractiveFileView(const String& filename) {
    // Save current screen  
    clearScreen();
    
    File file = FatFS.open(filename.c_str(), "r");
    if (!file) {
        moveCursor(5, 5);
        changeTerminalColor(FileColors::ERROR, false);
        Serial.print("Failed to open file: " + filename);
        changeTerminalColor(0, false);
        Serial.flush();
        delayMicroseconds(2000);
        drawInterface();
        return;
    }
    
    // Draw header
    moveCursor(1, 1);
    changeTerminalColor(FileColors::HEADER, false);
    Serial.println("╭───────────────────────────────────────────────────────────────────────────╮");
    Serial.println("│                              FILE VIEWER                                  │");
    Serial.println("╰───────────────────────────────────────────────────────────────────────────╯");
    
    moveCursor(4, 3);
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("File: ");
    Serial.println(filename);
    
    // Show file content (first 15 lines)
    changeTerminalColor(248, false); // Light grey for content
    int lineCount = 0;
    int row = 6;
    while (file.available() && lineCount < 15 && row < 21) {
        String line = file.readStringUntil('\n');
        moveCursor(row, 3);
        Serial.print(line.substring(0, 75)); // Truncate long lines
        row++;
        lineCount++;
    }
    
    if (file.available()) {
        moveCursor(21, 3);
        changeTerminalColor(FileColors::STATUS, false);
        Serial.print("... (file continues - press 'e' to edit full file)");
    }
    
    file.close();
    
    moveCursor(23, 3);
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("Press any key to return...");
    changeTerminalColor(0, false);
    Serial.flush();
    
    // Wait for keypress
    while (Serial.available() == 0) delayMicroseconds(100);
    while (Serial.available() > 0) Serial.read();
    
    // Redraw main interface
    drawInterface();
}

void FileManager::showInteractiveFileInfo(const FileEntry& file) {
    // Use a small info popup instead of full screen
    moveCursor(18, 5);
    clearCurrentLine();
    changeTerminalColor(FileColors::HEADER, false);
    Serial.print("Info: ");
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print(file.name);
    Serial.print(" | Size: ");
    if (file.isDirectory) {
        Serial.print("<DIR>");
    } else {
        Serial.print(formatFileSize(file.size));
    }
    if (file.lastModified != 0) {
        Serial.print(" | Modified: ");
        Serial.print(formatDateTime(file.lastModified));
    }
    changeTerminalColor(0, false);
    Serial.flush();
    
    // Also show on OLED if connected
    if (oled.oledConnected) {
        String infoText = file.name;
        if (file.isDirectory) {
            infoText += " <DIR>";
        } else {
            infoText += " " + formatFileSize(file.size);
        }
        oled.showMultiLineSmallText(infoText.c_str(), true);
        oled.restoreNormalFont();
    }
    
    // Clear info after 3 seconds
    delay(100); // Shortened from 3000ms
    moveCursor(18, 5);
    clearCurrentLine();
    Serial.flush();
}

// Utility functions
String getFullPath(const String& basePath, const String& filename) {
    if (basePath.endsWith("/")) {
        return basePath + filename;
    } else {
        return basePath + "/" + filename;
    }
}

bool isValidFilename(const String& filename) {
    if (filename.length() == 0) return false;
    
    // Check for invalid characters
    String invalidChars = "<>:\"|?*";
    for (int i = 0; i < invalidChars.length(); i++) {
        if (filename.indexOf(invalidChars[i]) >= 0) {
            return false;
        }
    }
    
    return true;
}

void printColoredPath(const String& path) {
    String segments = path;
    segments.replace("/", " > ");
    
    changeTerminalColor(FileColors::DIRECTORY, false);
    Serial.print(segments);
    changeTerminalColor(0, false);
}

// App entry points
void filesystemApp() {

    bool showOledInTerminal = jumperlessConfig.top_oled.show_in_terminal;
    jumperlessConfig.top_oled.show_in_terminal = false;
    
    
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n  Starting Jumperless File Manager...");
    Serial.println("   Navigate files and directories with colorful interface");
    Serial.println("   Create, edit, and manage files using eKilo editor\n\n\r");
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("   Press Enter to launch File Manager...");
    changeTerminalColor(8, true);
    FileManager manager;
    manager.initInteractiveMode();

        

    
    // Wait for user to press enter to break the input loop
    while (Serial.available() == 0) {
        delayMicroseconds(100);
    }
    // Clear the enter keypress
    while (Serial.available() > 0) {
        Serial.read();
    }
    
    // Save current screen state and switch to alternate screen buffer
    saveScreenState(&Serial);
    
    manager.clearScreen();
    manager.hideCursor();
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("   Initializing filesystem...");
    changeTerminalColor(8, true);
    delayMicroseconds(100); // Give time for initialization messages to be seen
    
    
    // Check if filesystem initialization was successful
    if (manager.getCurrentPath() == "[NO_FS]") {
        changeTerminalColor(FileColors::ERROR, true);
        Serial.println();
        Serial.println("   FILESYSTEM INITIALIZATION FAILED"); 
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("   The file manager cannot access the FatFS filesystem.");
        Serial.println("   This may be because:");
        Serial.println("   • FatFS is not properly initialized in the main firmware");
        Serial.println("   • No filesystem has been formatted on the flash storage");
        Serial.println("   • There is a hardware issue with the flash memory");
        Serial.println();
        Serial.println("   You can still use the file manager interface, but");
        Serial.println("   file operations will not be available until the");
        Serial.println("   filesystem is properly set up.");
        Serial.println();
        changeTerminalColor(FileColors::STATUS, true);
        Serial.print("   Press 'q' to quit or any other key to continue...");
        changeTerminalColor(0, false);
        
        while (Serial.available() == 0) {
            delayMicroseconds(10);
        }
        char c = Serial.read();
        Serial.println();
        
        if (c == 'q' || c == 'Q') {
            Serial.println("  File Manager cancelled.");
            return;
        }
    }
    
    manager.run();

    // Restore original screen state with all scrollback intact
    restoreScreenState(&Serial);

    jumperlessConfig.top_oled.show_in_terminal = showOledInTerminal;
}

void eKiloApp() {
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n  eKilo Text Editor");
    Serial.println("   Full-featured terminal text editor with syntax highlighting");
    Serial.println("   Perfect for editing MicroPython scripts and config files");
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("\n   Press Enter to launch eKilo Editor...");
    changeTerminalColor(-1, true);
    
    // Wait for user to press enter to break the input loop
    while (Serial.available() == 0) {
        delayMicroseconds(100);
    }
    // Clear the enter keypress
    while (Serial.available() > 0) {
        Serial.read();
    }
    
    // Save current screen state and switch to alternate screen buffer
    saveScreenState(&Serial);
    
    launchEkilo(nullptr);
    
    // Restore original screen state with all scrollback intact
    restoreScreenState(&Serial);
}

void launchEkilo(const char* filename) {
    // Store if we're called from file manager or standalone
    static bool calledFromFileManager = false;
    calledFromFileManager = (filename != nullptr);
    
    changeTerminalColor(FileColors::HEADER, false);
    if (filename) {
        Serial.println("\n\n\r⍺ Opening " + String(filename) + " in eKilo editor...");
    } else {
        Serial.println("\n\n\r⍺ Starting eKilo editor...");
    }
    Serial.print("\x1b[0m"); // Reset colors instead of setting to black
    
    // Flush any pending serial data before launching editor
    Serial.flush();
    while (Serial.available() > 0) {
        Serial.read();
    }
    
    // Brief pause to let user see the message
    delayMicroseconds(100);
    
    // Clear screen and prepare for full-screen editor
    Serial.print("\x1b[2J\x1b[H");
    
    // Launch eKilo editor
    int result = ekilo_main(filename);
    
    // Editor has exited - clean up and prepare for return
    // Clear screen immediately to prepare for next interface
    Serial.print("\x1b[2J\x1b[H");
    
    // Brief status message
    changeTerminalColor(FileColors::STATUS, false);
    if (result == 0) {
        Serial.println("☺ Editor session completed successfully");
        if (calledFromFileManager) {
            Serial.println("⌘ Returning to file manager...");
        } else {
            Serial.println("⌘ Returning to main menu...");
        }
    } else {
        Serial.println("☹ Editor session ended with error");
        if (calledFromFileManager) {
            Serial.println("⌘ Returning to file manager...");
        } else {
            Serial.println("⌘ Returning to main menu...");
        }
    }
    changeTerminalColor(-1, false); // Reset colors
    
    // Different handling based on how we were called
    if (calledFromFileManager) {
        // Called from file manager - just set flag, don't exit to main menu
        delayMicroseconds(100); // Give user time to see the message
    } else {
        // Called standalone (from Apps menu) - wait for user input
        Serial.println("Press any key to continue...");
        while (Serial.available() == 0) {
            delayMicroseconds(100);
        }
        while (Serial.available() > 0) {
            Serial.read();
        }
    }
    
    // Clear screen one more time to ensure clean return
    Serial.print("\x1b[2J\x1b[H");
}

// REPL mode version of eKilo - returns content if file was saved
String launchEkiloREPL(const char* filename) {
    String finalFilename = "";
    
    if (filename) {
        finalFilename = String(filename);
        changeTerminalColor(FileColors::HEADER, false);
        Serial.println("Opening " + finalFilename + " in eKilo editor...");
    } else {
        // Auto-generate filename using the same logic as ScriptHistory
        finalFilename = generateNextScriptName();
        changeTerminalColor(FileColors::HEADER, false);
        Serial.println("Creating " + finalFilename + " in eKilo editor...");
    }
    Serial.print("\x1b[0m"); // Reset colors
    
    // Save current screen state and switch to alternate screen buffer
    saveScreenState(&Serial);
    
    // Launch eKilo editor in REPL mode
    String savedContent = ekilo_main_repl(finalFilename.c_str());
    
    // Restore original screen state with all scrollback intact
    restoreScreenState(&Serial);
    
    changeTerminalColor(FileColors::STATUS, false);
    if (savedContent.length() > 0) {
        Serial.println("☺ File saved as " + finalFilename);
    } else {
        Serial.println("☺ Editor session completed");
    }
    changeTerminalColor(-1, false); // Reset colors
    
    return savedContent;
}

// Helper function to generate next script name
String generateNextScriptName() {
    String scripts_dir = "/python_scripts";
    
    // Create python_scripts directory if it doesn't exist
    if (!FatFS.exists(scripts_dir)) {
        FatFS.mkdir(scripts_dir);
    }
    
    // Find next available script number
    int next_script_number = 1;
    for (int i = 1; i <= 100; i++) {
        String test_script = scripts_dir + "/script_" + String(i) + ".py";
        if (FatFS.exists(test_script)) {
            next_script_number = i + 1;
        } else {
            break; // Found first gap, use it
        }
    }
    
    return scripts_dir + "/script_" + String(next_script_number) + ".py";
}

// Note: Simple text editor replaced with full eKilo implementation
// The following function is deprecated and will be removed
int jumperless_simple_editor_deprecated(const char* filename) {
    if (!filename) {
        changeTerminalColor(FileColors::ERROR, true);
        Serial.println("Error: No filename provided for editor");
        changeTerminalColor(0, true);
        return 1;
    }
    
    String fullPath = filename;
    if (!fullPath.startsWith("/")) {
        fullPath = "/" + fullPath;
    }
    
    changeTerminalColor(FileColors::HEADER, true);
    Serial.print("\r\n╭───────────────────────────────────────────────────────────────────────────╮\r\n");
    Serial.print("│                         JUMPERLESS TEXT EDITOR                            │\r\n");
    Serial.print("╰───────────────────────────────────────────────────────────────────────────╯\r\n");
    
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print("File: " + fullPath + "\r\n");
    Serial.print("Commands: :w = save, :q = quit, :wq = save & quit, :help = show help\r\n");
    Serial.print("╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\r\n");
    changeTerminalColor(0, true);
    
    // Load file content
    std::vector<String> lines;
    bool fileExists = FatFS.exists(fullPath.c_str());
    
    if (fileExists) {
        File file = FatFS.open(fullPath.c_str(), "r");
        if (file) {
            changeTerminalColor(155, false); // Green
            Serial.println("\n\n\r⌘ Loading existing file...");
            changeTerminalColor(0, false);
            
            while (file.available()) {
                String line = file.readStringUntil('\n');
                line.trim(); // Remove carriage returns
                lines.push_back(line);
            }
            file.close();
            
            if (lines.empty()) {
                lines.push_back(""); // Ensure at least one empty line
            }
        } else {
            changeTerminalColor(FileColors::ERROR, true);
            Serial.println("Error: Cannot open file for reading");
            changeTerminalColor(0, true);
            return 1;
        }
    } else {
        changeTerminalColor(221, false); // Yellow
        Serial.println("\n\n\r⍺ Creating new file...");
        changeTerminalColor(0, false);
        lines.push_back(""); // Start with one empty line
    }
    
    // Display initial content
    changeTerminalColor(248, false); // Light gray
    Serial.print("\r\n\n\rCurrent content:\r\n");
    for (int i = 0; i < lines.size(); i++) {
        Serial.printf("%3d: %s\r\n", i + 1, lines[i].c_str());
    }
    changeTerminalColor(0, false);
    
    bool modified = false;
    bool running = true;
    
    while (running) {
        changeTerminalColor(FileColors::STATUS, false);
        Serial.print("\r\nEditor> ");
        changeTerminalColor(0, false);
        Serial.flush();
        
        // Character-by-character input handling (like file manager)
        String input = "";
        bool inputComplete = false;
        
        while (!inputComplete) {
            if (Serial.available() > 0) {
                char c = Serial.read();
                
                if (c == '\r' || c == '\n') {
                    // Enter pressed - complete input
                    Serial.print("\r\n");
                    inputComplete = true;
                } else if (c == 127 || c == 8) {
                    // Backspace (127 = DEL, 8 = BS)
                    if (input.length() > 0) {
                        input.remove(input.length() - 1);
                        Serial.print("\b \b"); // Backspace, space, backspace
                    }
                } else if (c == 27) {
                    // ESC sequence - might be arrow keys, ignore for now
                    // Read and discard any following characters in the sequence
                    delayMicroseconds(10);
                    while (Serial.available() > 0) {
                        Serial.read();
                        delayMicroseconds(1);
                    }
                } else if (c >= 32 && c <= 126) {
                    // Printable character
                    input += c;
                    Serial.print(c); // Echo the character
                }
                // Ignore other control characters
            } else {
                delayMicroseconds(10);
            }
        }
        
        input.trim();
        
        if (input.startsWith(":")) {
            // Command mode
            if (input == ":q") {
                if (modified) {
                    changeTerminalColor(221, false); // Yellow
                    Serial.print("Warning: File has been modified. Use :q! to quit without saving or :wq to save and quit.\r\n");
                    changeTerminalColor(0, false);
                } else {
                    running = false;
                }
            } else if (input == ":q!") {
                running = false;
            } else if (input == ":w") {
                // Save file
                File file = FatFS.open(fullPath.c_str(), "w");
                if (file) {
                    for (int i = 0; i < lines.size(); i++) {
                        file.println(lines[i]);
                    }
                    file.close();
                    modified = false;
                    changeTerminalColor(155, false); // Green
                    Serial.print("☺ File saved successfully\r\n");
                    changeTerminalColor(0, false);
                } else {
                    changeTerminalColor(FileColors::ERROR, true);
                    Serial.print("❌ Error: Cannot save file\r\n");
                    changeTerminalColor(0, true);
                }
            } else if (input == ":wq") {
                // Save and quit
                File file = FatFS.open(fullPath.c_str(), "w");
                if (file) {
                    for (int i = 0; i < lines.size(); i++) {
                        file.println(lines[i]);
                    }
                    file.close();
                    changeTerminalColor(155, false); // Green
                    Serial.print("☺ File saved successfully\r\n");
                    changeTerminalColor(0, false);
                    running = false;
                } else {
                    changeTerminalColor(FileColors::ERROR, true);
                    Serial.print("❌ Error: Cannot save file\r\n");
                    changeTerminalColor(0, true);
                }
            } else if (input == ":help") {
                // eKilo has its own built-in help - this is placeholder for compatibility
                Serial.print("eKilo editor help: Use Ctrl+S to save, Ctrl+Q to quit, Arrow keys to navigate\r\n");
            } else if (input.startsWith(":d")) {
                // Delete line - :d5 deletes line 5
                String numStr = input.substring(2);
                numStr.trim();
                if (numStr.length() > 0) {
                    int lineNum = numStr.toInt();
                    if (lineNum > 0 && lineNum <= lines.size()) {
                        lines.erase(lines.begin() + lineNum - 1);
                        modified = true;
                        changeTerminalColor(221, false); // Yellow
                        Serial.print("Line " + String(lineNum) + " deleted\r\n");
                        changeTerminalColor(0, false);
                        
                        // Show updated content
                        changeTerminalColor(248, false);
                        Serial.print("\r\nUpdated content:\r\n");
                        for (int i = 0; i < lines.size(); i++) {
                            Serial.printf("%3d: %s\r\n", i + 1, lines[i].c_str());
                        }
                        changeTerminalColor(0, false);
                    } else {
                        changeTerminalColor(FileColors::ERROR, true);
                        Serial.print("Invalid line number\r\n");
                        changeTerminalColor(0, true);
                    }
                } else {
                    changeTerminalColor(FileColors::ERROR, true);
                    Serial.print("Usage: :d<line_number> (e.g., :d5)\r\n");
                    changeTerminalColor(0, true);
                }
            } else if (input == ":l") {
                // List all lines
                changeTerminalColor(248, false);
                Serial.print("\r\nCurrent content:\r\n");
                for (int i = 0; i < lines.size(); i++) {
                    Serial.printf("%3d: %s\r\n", i + 1, lines[i].c_str());
                }
                changeTerminalColor(0, false);
            } else {
                changeTerminalColor(FileColors::ERROR, true);
                Serial.print("Unknown command. Type :help for available commands.\r\n");
                changeTerminalColor(0, true);
            }
        } else if (input.length() > 0) {
            // Text input - add new line
            lines.push_back(input);
            modified = true;
            changeTerminalColor(155, false); // Green
            Serial.print("Line " + String(lines.size()) + " added\r\n");
            changeTerminalColor(0, false);
        }
    }
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.print("Exiting editor...\r\n");
    changeTerminalColor(0, true);
    
    return 0;
}

void show_simple_editor_help() {
    changeTerminalColor(FileColors::HEADER, true);
    Serial.print("\r\n╭───────────────────────────────────────────────────────────────────────────╮\r\n");
    Serial.print("│                         TEXT EDITOR HELP                                  │\r\n");
    Serial.print("╰───────────────────────────────────────────────────────────────────────────╯\r\n");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.print("\r\n⍺ EDITING:\r\n");
    changeTerminalColor(248, false);
    Serial.print("  Type any text and press Enter to add a new line\r\n");
    Serial.print("  Empty input (just Enter) does nothing\r\n");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.print("\r\n▣ COMMANDS:\r\n");
    changeTerminalColor(155, false); // Green
    Serial.print("  :w              ");
    changeTerminalColor(248, false);
    Serial.print("- Save file\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :q              ");
    changeTerminalColor(248, false);
    Serial.print("- Quit (warns if unsaved)\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :q!             ");
    changeTerminalColor(248, false);
    Serial.print("- Quit without saving\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :wq             ");
    changeTerminalColor(248, false);
    Serial.print("- Save and quit\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :l              ");
    changeTerminalColor(248, false);
    Serial.print("- List all lines with numbers\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :d<num>         ");
    changeTerminalColor(248, false);
    Serial.print("- Delete line number (e.g., :d5)\r\n");
    changeTerminalColor(155, false);
    Serial.print("  :help           ");
    changeTerminalColor(248, false);
    Serial.print("- Show this help\r\n");
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.print("\r\n○ TIPS:\r\n");
    changeTerminalColor(248, false);
    Serial.print("  • This is a simple line-based editor\r\n");
    Serial.print("  • Each line of text must be entered separately\r\n");
    Serial.print("  • Use :l to see line numbers for deletion\r\n");
    Serial.print("  • Commands start with ':' (colon)\r\n");
    
    changeTerminalColor(0, true);
}

bool FileManager::createDirectory(const String& dirname) {
    String fullPath = getFullPath(currentPath, dirname);
    
    if (FatFS.exists(fullPath.c_str())) {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Directory already exists: " + dirname);
        changeTerminalColor(-1, false);
        return false;
    }
    
    if (FatFS.mkdir(fullPath.c_str())) {
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("Created directory: " + dirname);
        changeTerminalColor(-1, false);
        refreshListing();
        return true;
    } else {
        changeTerminalColor(FileColors::ERROR, false);
        Serial.println("Failed to create directory: " + dirname);
        changeTerminalColor(-1, false);
        return false;
    }
}

String FileManager::promptForFilename(const String& prompt) {
    changeTerminalColor(FileColors::STATUS, false);
    Serial.print(prompt);
    changeTerminalColor(-1, false);
    
    String filename = "";
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\r' || c == '\n') {
                Serial.println();
                break;
            } else if (c == 8 || c == 127) { // Backspace
                if (filename.length() > 0) {
                    filename.remove(filename.length() - 1);
                    Serial.print("\b \b");
                }
            } else if (c == 27) { // ESC - cancel
                Serial.println("\n[Cancelled]");
                return "";
            } else if (c >= 32 && c <= 126) { // Printable characters
                filename += c;
                Serial.print(c);
            }
        }
        delayMicroseconds(10);
    }
    
    return filename;
}

void filesystemAppPythonScripts() {
    bool showOledInTerminal = jumperlessConfig.top_oled.show_in_terminal;
    jumperlessConfig.top_oled.show_in_terminal = false;
    
    changeTerminalColor(FileColors::HEADER, true);
    Serial.println("\n\r  Starting Jumperless File Manager (Python Scripts)...");
    Serial.println("   Navigate Python scripts with colorful interface");
    Serial.println("   Create, edit, and manage Python files using eKilo editor\n\n\r");
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("   Press Enter to launch File Manager...");
    changeTerminalColor(8, true);
    
    FileManager manager;
    manager.initInteractiveMode();
    
    // Wait for user to press enter to break the input loop
    while (Serial.available() == 0) {
        delayMicroseconds(100);
    }
    // Clear the enter keypress
    while (Serial.available() > 0) {
        Serial.read();
    }
    
    // Save current screen state and switch to alternate screen buffer
    saveScreenState(&Serial);
    
    //manager.clearScreen();
    //manager.hideCursor();
    
    changeTerminalColor(FileColors::STATUS, true);
    Serial.println("   Initializing filesystem...");
    changeTerminalColor(8, true);
    delayMicroseconds(100); // Give time for initialization messages to be seen
    
    // Check if filesystem initialization was successful
    if (manager.getCurrentPath() == "[NO_FS]") {
        changeTerminalColor(FileColors::ERROR, true);
        Serial.println();
        Serial.println("   FILESYSTEM INITIALIZATION FAILED"); 
        changeTerminalColor(FileColors::STATUS, false);
        Serial.println("   The file manager cannot access the FatFS filesystem.");
        Serial.println("   This may be because:");
        Serial.println("   • FatFS is not properly initialized in the main firmware");
        Serial.println("   • No filesystem has been formatted on the flash storage");
        Serial.println("   • There is a hardware issue with the flash memory");
        Serial.println();
        Serial.println("   You can still use the file manager interface, but");
        Serial.println("   file operations will not be available until the");
        Serial.println("   filesystem is properly set up.");
        Serial.println();
        changeTerminalColor(FileColors::STATUS, true);
        Serial.print("   Press 'q' to quit or any other key to continue...");
        changeTerminalColor(0, false);
        
        while (Serial.available() == 0) {
            delayMicroseconds(10);
        }
        char c = Serial.read();
        Serial.println();
        
        if (c == 'q' || c == 'Q') {
            Serial.println("  File Manager cancelled.");
            jumperlessConfig.top_oled.show_in_terminal = showOledInTerminal;
            return;
        }
    } else {
        // Try to navigate to python_scripts directory
        changeTerminalColor(FileColors::STATUS, true);
        //Serial.println("   Navigating to python_scripts directory...");
        changeTerminalColor(8, true);
        
        // Create python_scripts directory if it doesn't exist
        if (!FatFS.exists("/python_scripts")) {
            Serial.println("   Creating python_scripts directory...");
            if (FatFS.mkdir("/python_scripts")) {
                changeTerminalColor(FileColors::HEADER, false);
                Serial.println("   ☺ Created /python_scripts directory");
                changeTerminalColor(8, true);
            } else {
                changeTerminalColor(FileColors::ERROR, false);
                Serial.println("   ☹ Failed to create /python_scripts directory");
                Serial.println("   Starting in root directory instead...");
                changeTerminalColor(8, true);
            }
        }
        
        // Navigate to python_scripts directory
        if (FatFS.exists("/python_scripts")) {
            if (manager.changeDirectory("/python_scripts")) {
                changeTerminalColor(FileColors::HEADER, false);
                //Serial.println("   ☺ Navigated to /python_scripts");
                changeTerminalColor(8, true);
            } else {
                changeTerminalColor(FileColors::ERROR, false);
                Serial.println("   ☹ Failed to navigate to /python_scripts");
                Serial.println("   Starting in root directory instead...");
                changeTerminalColor(8, true);
            }
        }
    }
    
    manager.run();
    
    // Restore original screen state with all scrollback intact
    restoreScreenState(&Serial);
    
    jumperlessConfig.top_oled.show_in_terminal = showOledInTerminal;
}

// REPL mode version - returns content if file was saved
String filesystemAppPythonScriptsREPL() {
    // Clear screen for file manager interface
    saveScreenState(&Serial);
    
    FileManager manager;
    // Set REPL mode so the manager knows to use launchEkiloREPL
    manager.setREPLMode(true);
    
    // Use normal interactive mode instead of special REPL mode
    manager.initInteractiveMode();
    
    // Create python_scripts directory if it doesn't exist
    if (!FatFS.exists("/python_scripts")) {
        FatFS.mkdir("/python_scripts");
    }
    
    // Navigate to python_scripts directory
    if (FatFS.exists("/python_scripts")) {
        manager.changeDirectory("/python_scripts");
    }
    
    manager.run();
    String content = manager.getLastSavedFileContent();
    
    // Clear screen for clean return to REPL
    restoreScreenState(&Serial);
    
    return content;
}

String FileManager::getLastSavedFileContent() {
    return lastOpenedFileContent;
}

//==============================================================================
// Global Utility Functions for External Use (e.g., USB filesystem)
//==============================================================================

FileType getFileTypeFromFilename(const String& filename) {
    String lower = filename;
    lower.toLowerCase();
    
    if (lower.endsWith(".py") || lower.endsWith(".pyw") || lower.endsWith(".pyi")) {
        return FILE_TYPE_PYTHON;
    } else if (lower.endsWith(".json")) {
        return FILE_TYPE_JSON;
    } else if (lower.endsWith(".cfg") || lower.endsWith(".conf") || lower.startsWith("config") || filename == "config.txt") {
        return FILE_TYPE_CONFIG;
    } else if (lower.startsWith("nodefileslot") && lower.endsWith(".txt")) {
        return FILE_TYPE_NODEFILES;
    } else if (lower.startsWith("netcolorsslot") && lower.endsWith(".txt")) {
        return FILE_TYPE_COLORS;
    } else if (lower.endsWith(".txt") || lower.endsWith(".md") || lower.endsWith(".readme")) {
        return FILE_TYPE_TEXT;
    }
    return FILE_TYPE_UNKNOWN;
}

String getFileIconFromType(FileType type) {
    switch (type) {
        case FILE_TYPE_DIRECTORY: return "⌘";
        case FILE_TYPE_PYTHON: return "𓆚";
        case FILE_TYPE_TEXT: return "⍺";
        case FILE_TYPE_CONFIG: return "⚙";
        case FILE_TYPE_JSON: return "⟐";
        case FILE_TYPE_NODEFILES: return "☊";
        case FILE_TYPE_COLORS: return "⎃";
        default: return "⍺";
    }
}

String formatFileSizeForUSB(size_t size) {
    if (size < 1024) {
        return String(size) + " B";
    } else if (size < 1024 * 1024) {
        return String(size / 1024) + " KB";
    } else {
        return String(size / (1024 * 1024)) + " MB";
    }
}

