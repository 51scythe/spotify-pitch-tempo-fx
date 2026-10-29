using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: AssemblyTitle("Spotify Pitch Tempo FX Setup")]
[assembly: AssemblyDescription("Per-user installer for Spotify-only pitch, tempo and effects")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

internal static class Program
{
    internal static string Extract(string destination)
    {
        string root = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (Directory.Exists(root) && Directory.GetFileSystemEntries(root).Length != 0)
            throw new IOException("Extraction directory must be new or empty.");
        Directory.CreateDirectory(root);
        using (Stream resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        using (var archive = new ZipArchive(resource, ZipArchiveMode.Read))
        {
            foreach (var entry in archive.Entries)
            {
                string target = Path.GetFullPath(Path.Combine(root, entry.FullName));
                if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new IOException("Invalid package path.");
                if (entry.Name.Length == 0) { Directory.CreateDirectory(target); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                using (var input = entry.Open())
                using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write)) input.CopyTo(output);
            }
        }
        return root;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        // Release verification extracts the identical embedded payload without installation.
        if (args.Length == 2 && args[0] == "--extract-only")
        {
            try { Extract(args[1]); return 0; } catch { return 1; }
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        if (new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
        {
            MessageBox.Show("Close Setup and double-click it normally. Do not use Run as administrator.", "Spotify Pitch Tempo FX", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 1;
        }
        string folder = Path.Combine(Path.GetTempPath(), "SpotifyPitchTempo-Setup-" + Guid.NewGuid().ToString("N"));
        try
        {
            Application.Run(new SetupWindow(Extract(folder)));
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "Setup could not start", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
        finally
        {
            try { if (Directory.Exists(folder)) Directory.Delete(folder, true); } catch { }
        }
    }
}

internal sealed class SetupWindow : Form
{
    private readonly string payload;
    private readonly TextBox output = new TextBox();
    private readonly Label state = new Label();
    private readonly Button check = new Button(), install = new Button(), remove = new Button(), open = new Button();
    private bool running;

    internal SetupWindow(string folder)
    {
        payload = folder;
        Text = "Spotify Pitch, Tempo & FX - Setup 1.0.0";
        ClientSize = new Size(760, 565);
        MinimumSize = new Size(690, 540);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 10);
        BackColor = Color.FromArgb(24, 24, 24);
        ForeColor = Color.WhiteSmoke;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), ColumnCount = 1, RowCount = 7 };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 76));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 50));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        layout.Controls.Add(new Label { Text = "Pitch, Tempo & FX", UseMnemonic = false, Font = new Font("Segoe UI", 23, FontStyle.Bold), Dock = DockStyle.Fill }, 0, 0);
        layout.Controls.Add(new Label { Text = "Reverb, distortion, muffle, independent pitch and tempo.\r\nRequires Windows x64 + desktop Spotify 1.3.6.1921.\r\nQuit Spotify completely before Install / Repair or Remove.", Dock = DockStyle.Fill }, 0, 1);
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        Configure(check, "Check compatibility", 175);
        Configure(install, "Install / Repair", 160);
        Configure(remove, "Remove", 110);
        install.BackColor = Color.FromArgb(30, 215, 96); install.ForeColor = Color.Black;
        actions.Controls.AddRange(new Control[] { check, install, remove });
        layout.Controls.Add(actions, 0, 2);
        state.Text = "Ready. Your Spotify build is checked before installation changes.";
        state.Dock = DockStyle.Fill;
        layout.Controls.Add(state, 0, 3);
        output.Multiline = true; output.ReadOnly = true; output.ScrollBars = ScrollBars.Vertical;
        output.WordWrap = true; output.Dock = DockStyle.Fill;
        output.BackColor = Color.FromArgb(14, 14, 14); output.ForeColor = Color.Gainsboro;
        output.Font = new Font("Consolas", 9); output.BorderStyle = BorderStyle.FixedSingle;
        layout.Controls.Add(output, 0, 4);
        layout.Controls.Add(new Label { Text = "Per-user install. UI backups and a fresh private token on each new PC.", Dock = DockStyle.Fill, Padding = new Padding(0, 6, 0, 0), ForeColor = Color.Silver }, 0, 5);
        var footer = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        Configure(open, "Open Spotify + FX", 175); open.Enabled = false;
        var help = new Button(); Configure(help, "Instructions", 120);
        var logs = new Button(); Configure(logs, "Setup logs", 110);
        var close = new Button(); Configure(close, "Close", 95);
        footer.Controls.AddRange(new Control[] { open, help, logs, close });
        layout.Controls.Add(footer, 0, 6);
        Controls.Add(layout);
        check.Click += async delegate { await Run("Check"); };
        install.Click += async delegate { await Run("Install"); };
        remove.Click += async delegate { await Run("Remove"); };
        help.Click += delegate { Launch(Path.Combine(payload, "START-HERE.txt"), ""); };
        logs.Click += delegate {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SpotifyPitchTempo-setup-logs");
            Directory.CreateDirectory(path); Launch(path, "");
        };
        open.Click += delegate {
            Launch(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SpotifyPitchTempo", "SpotifyPitchTempo.exe"), "--launch --background");
        };
        close.Click += delegate { Close(); };
        FormClosing += delegate(object sender, FormClosingEventArgs e) { if (running) e.Cancel = true; };
    }

    private static void Configure(Button button, string text, int width)
    {
        button.Text = text; button.Width = width; button.Height = 36;
        button.Margin = new Padding(0, 3, 8, 3); button.FlatStyle = FlatStyle.Flat;
        button.BackColor = Color.FromArgb(45, 45, 45); button.ForeColor = Color.WhiteSmoke;
    }

    private void Launch(string path, string arguments)
    {
        try { Process.Start(new ProcessStartInfo(path, arguments) { UseShellExecute = true }); }
        catch (Exception e) { state.Text = e.Message; }
    }

    private void Log(string text)
    {
        if (text == null || IsDisposed) return;
        BeginInvoke((Action)delegate { output.AppendText(text + Environment.NewLine); });
    }

    private async Task Run(string action)
    {
        running = true; check.Enabled = install.Enabled = remove.Enabled = open.Enabled = false;
        output.Clear(); state.Text = action + " in progress...";
        state.ForeColor = Color.WhiteSmoke;
        try
        {
            int code = await Task.Run(delegate {
                string powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");
                var start = new ProcessStartInfo(powershell, "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Path.Combine(payload, "setup.ps1") + "\" -Action " + action);
                start.UseShellExecute = false; start.CreateNoWindow = true;
                start.RedirectStandardOutput = true; start.RedirectStandardError = true;
                start.StandardOutputEncoding = Encoding.UTF8; start.StandardErrorEncoding = Encoding.UTF8;
                using (var process = new Process { StartInfo = start })
                {
                    process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { Log(e.Data); };
                    process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { Log(e.Data); };
                    process.Start(); process.BeginOutputReadLine(); process.BeginErrorReadLine(); process.WaitForExit();
                    return process.ExitCode;
                }
            });
            state.Text = code == 0 ? (action == "Install" ? "Installed. Use Open Spotify + FX below; effects start off." : action + " completed successfully.") : action + " stopped. See the reason in the log below.";
            state.ForeColor = code == 0 ? Color.FromArgb(30, 215, 96) : Color.FromArgb(255, 195, 100);
            open.Enabled = code == 0 && action == "Install";
        }
        catch (Exception error) { state.Text = "Setup failed: " + error.Message; }
        finally { running = false; check.Enabled = install.Enabled = remove.Enabled = true; }
    }
}
