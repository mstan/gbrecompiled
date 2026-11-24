package org.gbrecompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.AtomicFile;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.*;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.zip.CRC32;

/** Private app-storage imports; SAF grants access without broad storage permissions. */
public final class SetupActivity extends Activity {
    private TextView status;
    private Button play;
    private File root;
    private String error="";
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        root=getFilesDir();
        LinearLayout layout=new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(24,24,24,24);
        TextView title=new TextView(this); title.setText(BuildConfig.GAME_TITLE); title.setTextSize(24); layout.addView(title);
        TextView help=new TextView(this);
        String instructions="Import your original game ROM. Setup remembers it for next time.";
        if(BuildConfig.NEEDS_BOOT_FIRMWARE) {
            instructions += BuildConfig.CGB_ONLY ? "\nImport the original CGB BIOS."
                : "\nImport the original DMG BIOS for the original game, or the CGB BIOS for DX.";
        }
        help.setText(instructions);
        layout.addView(help);
        button(layout,"Import game ROM",1);
        if(BuildConfig.NEEDS_BOOT_FIRMWARE) {
            if(!BuildConfig.CGB_ONLY)button(layout,"Import DMG BIOS (256 bytes)",2);
            button(layout,"Import CGB BIOS (2304 bytes)",3);
        }
        status=new TextView(this); layout.addView(status);
        play=new Button(this); play.setText("Open launcher"); layout.addView(play);
        play.setOnClickListener(v -> launch());
        setContentView(layout);
        refresh();
    }
    private void button(LinearLayout layout,String text,int code) {
        Button b=new Button(this); b.setText(text); layout.addView(b);
        b.setOnClickListener(v -> {
            Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(pick,code);
        });
    }
    private static byte[] bytes(InputStream input,int maximum) throws IOException {
        try(InputStream in=input;ByteArrayOutputStream out=new ByteArrayOutputStream()) {
            byte[] chunk=new byte[8192]; int n;
            while((n=in.read(chunk))!=-1) { if(out.size()+n>maximum)throw new IOException("Selected file is too large"); out.write(chunk,0,n); }
            return out.toByteArray();
        }
    }
    private static String sha(byte[] image) throws Exception {
        StringBuilder text=new StringBuilder();
        for(byte b:MessageDigest.getInstance("SHA-256").digest(image))text.append(String.format(Locale.ROOT,"%02x",b&255));
        return text.toString();
    }
    private File imported(int code) { return new File(root,code==1?"game.rom":code==2?"dmg_boot.bin":"cgb_boot.bin"); }
    private boolean valid(int code,byte[] image) throws Exception {
        if(code==1) {
            CRC32 crc=new CRC32();crc.update(image);
            return image.length==BuildConfig.ROM_SIZE && String.format(Locale.ROOT,"%08x",crc.getValue()).equalsIgnoreCase(BuildConfig.ROM_CRC);
        }
        return image.length==(code==2?256:2304) && sha(image).equals(code==2?BuildConfig.DMG_SHA:BuildConfig.CGB_SHA);
    }
    private boolean exists(int code) {
        try { return valid(code,bytes(new FileInputStream(imported(code)),BuildConfig.ROM_SIZE)); }
        catch(Exception failure) { return false; }
    }
    private static void write(File file,byte[] data) throws IOException {
        AtomicFile target=new AtomicFile(file); FileOutputStream stream=null;
        try { stream=target.startWrite();stream.write(data);target.finishWrite(stream); }
        catch(IOException failure) { if(stream!=null)target.failWrite(stream);throw failure; }
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK || data==null || data.getData()==null)return;
        try {
            Uri uri=data.getData();byte[] image=bytes(getContentResolver().openInputStream(uri),BuildConfig.ROM_SIZE);
            if(!valid(request,image))throw new IOException(request==1?"ROM size/CRC does not match this game.":"BIOS size/SHA-256 does not match this build.");
            write(imported(request),image);error="";
        } catch(Exception failure) { error=failure.getMessage(); }
        refresh();
    }
    private void refresh() {
        boolean rom=exists(1);
        String summary="ROM: "+(rom?"ready":"missing");
        boolean ready=rom;
        if(BuildConfig.NEEDS_BOOT_FIRMWARE) {
            boolean dmg=exists(2),cgb=exists(3);
            if(!BuildConfig.CGB_ONLY)summary+="\nDMG BIOS: "+(dmg?"ready":"missing");
            summary+="\nCGB BIOS: "+(cgb?"ready":"missing");
            ready=ready && (BuildConfig.CGB_ONLY?cgb:dmg||cgb);
        }
        status.setText(summary+"\n"+error);
        play.setEnabled(ready);
    }
    private void copyAsset(String name,File destination) throws IOException {
        String[] children=getAssets().list(name);
        if(children!=null && children.length>0) {
            if(!destination.isDirectory() && !destination.mkdirs())throw new IOException("Cannot create asset directory");
            for(String child:children)copyAsset(name+"/"+child,new File(destination,child));
        } else write(destination,bytes(getAssets().open(name),32*1024*1024));
    }
    private void launch() {
        try {
            copyAsset("payload",root);
            write(new File(root,"rom.cfg"),(imported(1).getAbsolutePath()+"\n").getBytes("UTF-8"));
            if(BuildConfig.NEEDS_BOOT_FIRMWARE) {
                for(int code=2;code<=3;code++)if(exists(code))write(new File(root,code==2?"bios_dmg.cfg":"bios_cgb.cfg"),(imported(code).getAbsolutePath()+"\n").getBytes("UTF-8"));
            }
            startActivity(new Intent(this,GameActivity.class));
        } catch(Exception failure) { new AlertDialog.Builder(this).setMessage(failure.getMessage()).setPositiveButton("OK",null).show(); }
    }
}
