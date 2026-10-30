import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import java.io.*;
// args: out-file addr count : print qwords (pointers) at addr
public class Qw extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs();
    try (PrintWriter w=new PrintWriter(a[0])) {
      Address ad=toAddr(a[1]); int n=Integer.parseInt(a[2]);
      for(int i=0;i<n;i++){ long v=currentProgram.getMemory().getLong(ad.add(8L*i)); w.println(ad.add(8L*i)+"  "+Long.toHexString(v)); }
    }
  }
}
