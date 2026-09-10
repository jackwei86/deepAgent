' DeepAgent 托盘静默启动器：双击本文件，不出现任何命令行窗口
' (直接以隐藏窗口方式拉起 pythonw 运行 tray.py)
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")

' 项目根 = 本脚本所在目录(assistant)的上一级
root = fso.GetParentFolderName(fso.GetParentFolderName(WScript.ScriptFullName))

pythonw = root & "\.venv\Scripts\pythonw.exe"
tray = root & "\assistant\tray.py"

If Not fso.FileExists(pythonw) Then
    MsgBox "未找到 " & pythonw & vbCrLf & "请先运行 assistant\scripts\setup_env.bat 安装环境。", 16, "DeepAgent"
    WScript.Quit 1
End If

' 参数含义: Run(命令, 窗口样式 0=完全隐藏, 是否等待 False)
sh.Run """" & pythonw & """ """ & tray & """", 0, False
