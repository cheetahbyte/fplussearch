class Fplussearch < Formula
  desc "File name, code symbol, and content search for macOS"
  homepage "https://github.com/cheetahbyte/fplussearch"
  license "MIT"
  head "https://github.com/cheetahbyte/fplussearch.git", branch: "main"

  depends_on "pcre2" => :build
  depends_on "pkgconf" => :build
  depends_on arch: :arm64
  depends_on :macos

  def install
    pcre2 = formula_opt_prefix("pcre2")
    system "make", "CXX=#{ENV.cxx}",
                   "CXXFLAGS=-std=c++20 -O3 -Wall -Wextra -Wpedantic -isystem #{pcre2}/include",
                   "PCRE2=#{pcre2}"
    bin.install "build/fplussearch"
  end

  service do
    run [opt_bin/"fplussearch", "serve"]
    keep_alive true
    log_path var/"log/fplussearch.log"
    error_log_path var/"log/fplussearch.log"
  end

  def caveats
    <<~EOS
      Swift clients start the shared daemon automatically when needed.
      To manage startup at login, use brew services start fplussearch.
      Do not also use fplussearch install --login, which installs a separate copy.
      For protected folders, grant #{opt_bin}/fplussearch Full Disk Access.
      Run brew services without sudo: indexes and sockets belong to your user.
    EOS
  end

  test do
    (testpath/"fixtures").mkpath
    (testpath/"fixtures/needle.txt").write("hello\n")
    assert_match "needle.txt", shell_output(
      "#{bin}/fplussearch --no-daemon --no-rescan --root #{testpath}/fixtures needle",
    )
  end
end
