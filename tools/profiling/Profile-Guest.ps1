param([string]$Link, [int]$ResponseSize, [int]$Version, [string]$BackendHost = '10.0.2.2', [int]$Port = 18080)
$ErrorActionPreference = 'Stop'
$Out = "C:\blorgfs-ci\prof\$Link"
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Text; using System.Threading; using System.Diagnostics;
using System.Collections.Generic; using System.Net.Sockets;
using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class Prof {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern SafeFileHandle CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, int il, byte[] o, int ol, out int r, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool ReadFile(SafeFileHandle h, IntPtr buf, int len, out int read, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern IntPtr VirtualAlloc(IntPtr a, UIntPtr size, uint type, uint prot);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool SetFilePointerEx(SafeFileHandle h, long dist, IntPtr newPos, uint method);

  static SafeFileHandle Dev() {
    var h = CreateFileW(@"\\.\BlorgFS", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h.IsInvalid) throw new Exception("open \\\\.\\BlorgFS failed: " + Marshal.GetLastWin32Error());
    return h;
  }
  public static void Reset() {
    using (var h = Dev()) { int r; if (!DeviceIoControl(h, 0x22A008, null, 0, null, 0, out r, IntPtr.Zero)) throw new Exception("reset failed: " + Marshal.GetLastWin32Error()); }
  }
  public static void Query(string path, int size, int version) {
    var b = new byte[size]; BitConverter.GetBytes(version).CopyTo(b, 0); BitConverter.GetBytes(size).CopyTo(b, 4);
    using (var h = Dev()) { int r; if (!DeviceIoControl(h, 0x226004, b, size, b, size, out r, IntPtr.Zero)) throw new Exception("query failed: " + Marshal.GetLastWin32Error()); }
    File.WriteAllBytes(path, b);
  }

  public static List<double> Lat = new List<double>();
  public static long Late;
  static double Ms(long ticks) { return ticks * 1000.0 / Stopwatch.Frequency; }
  static void Add(double ms) { lock (Lat) { Lat.Add(ms); } }
  public static string Summary() {
    List<double> l; lock (Lat) { l = new List<double>(Lat); }
    if (l.Count == 0) { return "n=0"; }
    l.Sort(); double sum = 0; foreach (var v in l) { sum += v; }
    Func<double, double> p = q => l[Math.Min(l.Count - 1, (int)Math.Floor(q * l.Count))];
    return String.Format("n={0} mean={1:F3}ms p50={2:F3} p90={3:F3} p99={4:F3} max={5:F3} late={6}",
      l.Count, sum / l.Count, p(0.5), p(0.9), p(0.99), l[l.Count - 1], Late);
  }

  // The same request bytes the driver sends (Client.c), on one keep-alive
  // connection: what the link and server cost with no filesystem in the way.
  class Raw {
    TcpClient c; NetworkStream s; byte[] hdr = new byte[8192]; byte[] body = new byte[4 << 20]; string host, path;
    public Raw(string h, int port, string p) { c = new TcpClient(); c.NoDelay = true; c.Connect(h, port); s = c.GetStream(); host = h; path = p; }
    public void Get(long off, int len) {
      var req = Encoding.ASCII.GetBytes(String.Format("GET /get_file?path={0} HTTP/1.1\r\nHost: {1}\r\nConnection: keep-alive\r\nRange: bytes={2}-{3}\r\n\r\n", path, host, off, off + len - 1));
      s.Write(req, 0, req.Length);
      int n = 0, end = -1;
      while (end < 0) {
        int r = s.Read(hdr, n, hdr.Length - n); if (r <= 0) throw new Exception("raw: connection closed");
        n += r;
        for (int i = 3; i < n; i++) { if (hdr[i-3] == 13 && hdr[i-2] == 10 && hdr[i-1] == 13 && hdr[i] == 10) { end = i + 1; break; } }
      }
      string h = Encoding.ASCII.GetString(hdr, 0, end);
      if (h.IndexOf(" 206 ") < 0) throw new Exception("raw: " + h.Split('\r')[0]);
      int cl = -1;
      foreach (var line in h.Split('\n')) { if (line.ToLowerInvariant().StartsWith("content-length:")) { cl = int.Parse(line.Substring(15).Trim()); } }
      int got = n - end;
      while (got < cl) { int r = s.Read(body, 0, Math.Min(body.Length, cl - got)); if (r <= 0) throw new Exception("raw: short body"); got += r; }
    }
    public void Close() { c.Close(); }
  }
  public static long RawSerial(string h, int port, string p, long start, long total, int chunk) {
    var r = new Raw(h, port, p); long done = 0;
    while (done < total) { var t = Stopwatch.GetTimestamp(); r.Get(start + done, chunk); Add(Ms(Stopwatch.GetTimestamp() - t)); done += chunk; }
    r.Close(); return done;
  }
  public static long RawRandom(string h, int port, string p, long fileLen, int count, int chunk, int seed) {
    var r = new Raw(h, port, p); var rng = new Random(seed); long done = 0; long blocks = fileLen / chunk;
    for (int i = 0; i < count; i++) { long off = (long)(rng.NextDouble() * blocks) * chunk; var t = Stopwatch.GetTimestamp(); r.Get(off, chunk); Add(Ms(Stopwatch.GetTimestamp() - t)); done += chunk; }
    r.Close(); return done;
  }
  public static long RawParallel(string h, int port, string p, int conns, long each, int chunk) {
    var ts = new List<Thread>(); long total = 0;
    for (int i = 0; i < conns; i++) { long st = i * each; var t = new Thread(() => { long d = RawSerial(h, port, p, st, each, chunk); Interlocked.Add(ref total, d); }); t.Start(); ts.Add(t); }
    foreach (var t in ts) { t.Join(); }
    return total;
  }

  public static long SeqBuffered(string path, int buf) {
    var b = new byte[buf]; long total = 0;
    using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.SequentialScan)) {
      while (true) { var t = Stopwatch.GetTimestamp(); int n = f.Read(b, 0, b.Length); Add(Ms(Stopwatch.GetTimestamp() - t)); if (n <= 0) { break; } total += n; }
    }
    return total;
  }
  public static long SeqUnbuffered(string path, int chunk, long max) {
    var h = CreateFileW(path, 0x80000000, 1, IntPtr.Zero, 3, 0x20000000 | 0x08000000, IntPtr.Zero);
    if (h.IsInvalid) throw new Exception("unbuffered open failed: " + Marshal.GetLastWin32Error());
    var buf = VirtualAlloc(IntPtr.Zero, (UIntPtr)(uint)chunk, 0x3000, 4); long total = 0;
    using (h) {
      while (total < max) { int n; var t = Stopwatch.GetTimestamp(); if (!ReadFile(h, buf, chunk, out n, IntPtr.Zero)) throw new Exception("unbuffered read failed: " + Marshal.GetLastWin32Error()); Add(Ms(Stopwatch.GetTimestamp() - t)); if (n <= 0) { break; } total += n; }
    }
    return total;
  }
  public static long RandomBuffered(string path, int count, int size, int seed) {
    var buf = new byte[size]; var rng = new Random(seed); long total = 0;
    using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.RandomAccess)) {
      long blocks = f.Length / size;
      for (int i = 0; i < count; i++) { f.Position = (long)(rng.NextDouble() * blocks) * size; var t = Stopwatch.GetTimestamp(); total += f.Read(buf, 0, size); Add(Ms(Stopwatch.GetTimestamp() - t)); }
    }
    return total;
  }
  // A player: one read every interval, against a deadline of that interval.
  public static long Paced(string path, int chunk, int intervalMs, int seconds) {
    var b = new byte[chunk]; long total = 0; Late = 0;
    var clock = Stopwatch.StartNew(); long next = 0;
    using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.SequentialScan)) {
      while (clock.ElapsedMilliseconds < seconds * 1000L) {
        long wait = next - clock.ElapsedMilliseconds; if (wait > 0) { Thread.Sleep((int)wait); }
        var t = Stopwatch.GetTimestamp(); int n = f.Read(b, 0, chunk); double ms = Ms(Stopwatch.GetTimestamp() - t); Add(ms);
        if (ms > intervalMs) { Late++; }
        if (n <= 0) { break; } total += n; next += intervalMs;
      }
    }
    return total;
  }
  public static long SmallFiles(string[] paths) {
    var b = new byte[65536]; long total = 0;
    foreach (var p in paths) {
      var t = Stopwatch.GetTimestamp();
      using (var f = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.Read, 4096)) { int n; while ((n = f.Read(b, 0, b.Length)) > 0) { total += n; } }
      Add(Ms(Stopwatch.GetTimestamp() - t));
    }
    return total;
  }
  public static long Stat(string[] paths) {
    foreach (var p in paths) { var t = Stopwatch.GetTimestamp(); var a = File.GetAttributes(p); Add(Ms(Stopwatch.GetTimestamp() - t)); }
    return 0;
  }
  public static long ListDir(string dir, int times) {
    long n = 0;
    for (int i = 0; i < times; i++) { var t = Stopwatch.GetTimestamp(); n += Directory.GetFileSystemEntries(dir).Length; Add(Ms(Stopwatch.GetTimestamp() - t)); }
    return 0;
  }
  public static long Concurrent(string[] paths, int buf) {
    var ts = new List<Thread>(); long total = 0;
    foreach (var p in paths) { var pp = p; var t = new Thread(() => { long d = SeqBuffered(pp, buf); Interlocked.Add(ref total, d); }); t.Start(); ts.Add(t); }
    foreach (var t in ts) { t.Join(); }
    return total;
  }

  static PerformanceCounter[] cpu;
  public static void CpuStart() {
    cpu = new PerformanceCounter[] {
      new PerformanceCounter("Processor", "% Processor Time", "_Total"),
      new PerformanceCounter("Processor", "% Privileged Time", "_Total"),
      new PerformanceCounter("Processor", "% DPC Time", "_Total"),
      new PerformanceCounter("Processor", "% Interrupt Time", "_Total") };
    foreach (var c in cpu) { c.NextValue(); }
  }
  public static string CpuEnd() {
    return String.Format("cpu total={0:F1}% kernel={1:F1}% dpc={2:F1}% intr={3:F1}%", cpu[0].NextValue(), cpu[1].NextValue(), cpu[2].NextValue(), cpu[3].NextValue());
  }
}
'@
$log = Join-Path $Out 'timings.txt'
"profile $Link; guest RAM {0:N1} GB, {1} CPUs" -f ((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB), [Environment]::ProcessorCount | Set-Content $log
$deadline = (Get-Date).AddMinutes(3)
while (-not (Test-Path "B:\prof\$Link\big.bin")) { if ((Get-Date) -gt $deadline) { throw "B:\prof\$Link\big.bin never appeared" }; Start-Sleep 2 }
$enc = "%5Cprof%5C$Link%5Craw.bin"
$step = 0
function Step([string]$Name, [scriptblock]$Body) {
  $script:step++
  [Prof]::Lat.Clear(); [Prof]::Late = 0
  [Prof]::Reset(); [Prof]::CpuStart()
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $bytes = & $Body
  $sw.Stop()
  $cpu = [Prof]::CpuEnd()
  $tag = '{0:D2}-{1}' -f $script:step, $Name
  [Prof]::Query((Join-Path $Out "$tag.bin"), $ResponseSize, $Version)
  $mib = ($bytes | Measure-Object -Sum).Sum / 1MB
  "{0,-26} {1,8:N1} MiB {2,7:N2} s {3,8:N1} MiB/s | {4} | {5}" -f $tag, $mib, $sw.Elapsed.TotalSeconds, ($mib / [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)), [Prof]::Summary(), $cpu | Tee-Object -FilePath $log -Append
}
$d = "B:\prof\$Link"
$small = 0..299 | ForEach-Object { "$d\small\f$_.bin" }
Step 'raw-random-64k'     { [Prof]::RawRandom($BackendHost, $Port, $enc, 256MB, 300, 65536, 1) }
Step 'raw-serial-128k'    { [Prof]::RawSerial($BackendHost, $Port, $enc, 0, 256MB, 131072) }
Step 'raw-serial-1m'      { [Prof]::RawSerial($BackendHost, $Port, $enc, 0, 256MB, 1048576) }
Step 'raw-4conn-1m'       { [Prof]::RawParallel($BackendHost, $Port, $enc, 4, 64MB, 1048576) }
Step 'fs-unbuf-64k'       { [Prof]::SeqUnbuffered("$d\unb.bin", 65536, 64MB) }
Step 'fs-unbuf-128k'      { [Prof]::SeqUnbuffered("$d\unb.bin", 131072, 256MB) }
Step 'fs-seq-buffered'    { [Prof]::SeqBuffered("$d\big.bin", 1MB) }
Step 'fs-seq-rewarm'      { [Prof]::SeqBuffered("$d\big.bin", 1MB) }
Step 'fs-random-4k'       { [Prof]::RandomBuffered("$d\rand.bin", 400, 4096, 3) }
Step 'fs-random-256k'     { [Prof]::RandomBuffered("$d\rand.bin", 200, 262144, 5) }
Step 'fs-paced-6mbs'      { [Prof]::Paced("$d\paced.bin", 262144, 40, 20) }
Step 'fs-4-streams'       { [Prof]::Concurrent(@("$d\c0.bin", "$d\c1.bin", "$d\c2.bin", "$d\c3.bin"), 1MB) }
Step 'fs-list-300-cold'   { [Prof]::ListDir("$d\small", 1) }
Step 'fs-list-300-x20'    { [Prof]::ListDir("$d\small", 20) }
Step 'fs-open-small-cold' { [Prof]::SmallFiles($small) }
Step 'fs-open-small-warm' { [Prof]::SmallFiles($small) }
Start-Sleep -Seconds 5
Step 'fs-stat-after-5s'   { [Prof]::Stat($small) }
