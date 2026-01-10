Write-Host "▶ ZXing Scanner installer (Windows)"

# 1. Check MSYS2
$msys = "C:\msys64"
if (!(Test-Path $msys)) {
    Write-Host "📦 Installing MSYS2..."
    Invoke-WebRequest https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64.exe -OutFile msys2.exe
    Start-Process msys2.exe -Wait
}

$bash = "$msys\mingw64.exe"

# 2. Create build script
$script = @"
pacman -Syu --noconfirm
pacman -S --noconfirm \
 mingw-w64-x86_64-toolchain \
 mingw-w64-x86_64-opencv \
 mingw-w64-x86_64-zxing-cpp \
 mingw-w64-x86_64-glfw \
 mingw-w64-x86_64-pkg-config

cd /c/$(Get-Location | Select-Object -ExpandProperty Path | ForEach-Object { $_ -replace '\\','/' })

g++ main.cpp \
 imgui/imgui.cpp \
 imgui/imgui_draw.cpp \
 imgui/imgui_tables.cpp \
 imgui/imgui_widgets.cpp \
 imgui/backends/imgui_impl_glfw.cpp \
 imgui/backends/imgui_impl_opengl3.cpp \
 -Iimgui \
 -Iimgui/backends \
 -std=c++17 -O2 -O2 -Wall \
 $(pkg-config --cflags --libs opencv4 zxing glfw3) \
 -lopengl32 \
 -o scanner.exe
"@

$script | Out-File build.sh -Encoding ASCII

# 3. Run build
Start-Process $bash "-c ./build.sh" -Wait

Write-Host "✅ Done!"
Write-Host "▶ Run scanner.exe"