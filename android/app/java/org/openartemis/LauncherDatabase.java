package org.openartemis;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * 启动器数据库助手
 * 管理游戏历史记录、封面缓存和单人配置
 */
public class LauncherDatabase extends SQLiteOpenHelper {

    private static final String DATABASE_NAME = "krkr_launcher.db";
    private static final int DATABASE_VERSION = 1;

    // 历史记录表
    public static final String TABLE_HISTORY = "game_history";
    public static final String COL_HISTORY_ID = "_id";
    public static final String COL_HISTORY_NAME = "game_name";
    public static final String COL_HISTORY_PATH = "game_path";
    public static final String COL_HISTORY_IS_URI = "is_uri";
    public static final String COL_HISTORY_COVER_PATH = "cover_path";
    public static final String COL_HISTORY_LAST_PLAYED = "last_played";
    public static final String COL_HISTORY_PLAY_COUNT = "play_count";
    // 单人配置存储为JSON字符串
    public static final String COL_HISTORY_CONFIG_JSON = "config_json";

    private static final String CREATE_TABLE_HISTORY =
            "CREATE TABLE " + TABLE_HISTORY + " (" +
                    COL_HISTORY_ID + " INTEGER PRIMARY KEY AUTOINCREMENT, " +
                    COL_HISTORY_NAME + " TEXT NOT NULL, " +
                    COL_HISTORY_PATH + " TEXT NOT NULL, " +
                    COL_HISTORY_IS_URI + " INTEGER DEFAULT 0, " +
                    COL_HISTORY_COVER_PATH + " TEXT, " +
                    COL_HISTORY_LAST_PLAYED + " INTEGER, " +
                    COL_HISTORY_PLAY_COUNT + " INTEGER DEFAULT 0, " +
                    COL_HISTORY_CONFIG_JSON + " TEXT, " +
                    "UNIQUE(" + COL_HISTORY_PATH + ")" +
                    ")";

    private static LauncherDatabase sInstance;

    public static synchronized LauncherDatabase getInstance(Context context) {
        if (sInstance == null) {
            sInstance = new LauncherDatabase(context.getApplicationContext());
        }
        return sInstance;
    }

    private LauncherDatabase(Context context) {
        super(context, DATABASE_NAME, null, DATABASE_VERSION);
    }

    @Override
    public void onCreate(SQLiteDatabase db) {
        db.execSQL(CREATE_TABLE_HISTORY);
    }

    @Override
    public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        db.execSQL("DROP TABLE IF EXISTS " + TABLE_HISTORY);
        onCreate(db);
    }

    // ========== 历史记录操作 ==========

    /**
     * 记录游戏启动
     */
    public void recordGameLaunch(String gameName, String gamePath, boolean isUri) {
        SQLiteDatabase db = getWritableDatabase();
        long now = System.currentTimeMillis();

        ContentValues values = new ContentValues();
        values.put(COL_HISTORY_NAME, gameName);
        values.put(COL_HISTORY_PATH, gamePath);
        values.put(COL_HISTORY_IS_URI, isUri ? 1 : 0);
        values.put(COL_HISTORY_LAST_PLAYED, now);

        // 尝试更新现有记录，不存在则插入
        int rows = db.update(TABLE_HISTORY, values,
                COL_HISTORY_PATH + " = ?",
                new String[]{gamePath});

        if (rows == 0) {
            values.put(COL_HISTORY_PLAY_COUNT, 1);
            db.insert(TABLE_HISTORY, null, values);
        } else {
            // 增加游玩次数
            db.execSQL("UPDATE " + TABLE_HISTORY +
                    " SET " + COL_HISTORY_PLAY_COUNT + " = " +
                    COL_HISTORY_PLAY_COUNT + " + 1, " +
                    COL_HISTORY_LAST_PLAYED + " = ?" +
                    " WHERE " + COL_HISTORY_PATH + " = ?",
                    new String[]{String.valueOf(now), gamePath});
        }
    }

    /**
     * 获取历史记录列表（按最后游玩时间降序）
     */
    public List<HistoryEntry> getHistory(int limit) {
        List<HistoryEntry> list = new ArrayList<>();
        SQLiteDatabase db = getReadableDatabase();

        Cursor cursor = db.query(TABLE_HISTORY, null,
                null, null, null, null,
                COL_HISTORY_LAST_PLAYED + " DESC",
                limit > 0 ? String.valueOf(limit) : null);

        if (cursor != null) {
            while (cursor.moveToNext()) {
                HistoryEntry entry = new HistoryEntry();
                entry.id = cursor.getLong(cursor.getColumnIndexOrThrow(COL_HISTORY_ID));
                entry.gameName = cursor.getString(cursor.getColumnIndexOrThrow(COL_HISTORY_NAME));
                entry.gamePath = cursor.getString(cursor.getColumnIndexOrThrow(COL_HISTORY_PATH));
                entry.isUri = cursor.getInt(cursor.getColumnIndexOrThrow(COL_HISTORY_IS_URI)) == 1;
                entry.coverPath = cursor.getString(cursor.getColumnIndexOrThrow(COL_HISTORY_COVER_PATH));
                entry.lastPlayed = cursor.getLong(cursor.getColumnIndexOrThrow(COL_HISTORY_LAST_PLAYED));
                entry.playCount = cursor.getInt(cursor.getColumnIndexOrThrow(COL_HISTORY_PLAY_COUNT));
                entry.configJson = cursor.getString(cursor.getColumnIndexOrThrow(COL_HISTORY_CONFIG_JSON));
                list.add(entry);
            }
            cursor.close();
        }
        return list;
    }

    /**
     * 获取所有历史记录
     */
    public List<HistoryEntry> getAllHistory() {
        return getHistory(0);
    }

    /**
     * 删除单条历史记录
     */
    public void deleteHistory(long id) {
        SQLiteDatabase db = getWritableDatabase();
        db.delete(TABLE_HISTORY, COL_HISTORY_ID + " = ?",
                new String[]{String.valueOf(id)});
    }

    /**
     * 清空所有历史记录
     */
    public void clearAllHistory() {
        SQLiteDatabase db = getWritableDatabase();
        db.delete(TABLE_HISTORY, null, null);
    }

    // ========== 封面操作 ==========

    /**
     * 获取游戏封面位图
     */
    public Bitmap getCover(String gamePath) {
        SQLiteDatabase db = getReadableDatabase();
        Cursor cursor = db.query(TABLE_HISTORY,
                new String[]{COL_HISTORY_COVER_PATH},
                COL_HISTORY_PATH + " = ?",
                new String[]{gamePath},
                null, null, null);

        if (cursor != null && cursor.moveToFirst()) {
            String coverPath = cursor.getString(0);
            cursor.close();
            if (coverPath != null) {
                return BitmapFactory.decodeFile(coverPath);
            }
        } else if (cursor != null) {
            cursor.close();
        }
        return null;
    }

    /**
     * 保存封面路径
     */
    public void saveCoverPath(String gamePath, String coverPath) {
        SQLiteDatabase db = getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put(COL_HISTORY_COVER_PATH, coverPath);
        db.update(TABLE_HISTORY, values,
                COL_HISTORY_PATH + " = ?",
                new String[]{gamePath});
    }

    // ========== 单人配置操作 ==========

    /**
     * 获取游戏单人配置
     */
    public JSONObject getGameConfig(String gamePath) {
        SQLiteDatabase db = getReadableDatabase();
        Cursor cursor = db.query(TABLE_HISTORY,
                new String[]{COL_HISTORY_CONFIG_JSON},
                COL_HISTORY_PATH + " = ?",
                new String[]{gamePath},
                null, null, null);

        if (cursor != null && cursor.moveToFirst()) {
            String json = cursor.getString(0);
            cursor.close();
            if (json != null) {
                try {
                    return new JSONObject(json);
                } catch (JSONException e) {
                    e.printStackTrace();
                }
            }
        } else if (cursor != null) {
            cursor.close();
        }
        return new JSONObject();
    }

    /**
     * 保存游戏单人配置
     */
    public void saveGameConfig(String gamePath, JSONObject config) {
        SQLiteDatabase db = getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put(COL_HISTORY_CONFIG_JSON, config.toString());

        int rows = db.update(TABLE_HISTORY, values,
                COL_HISTORY_PATH + " = ?",
                new String[]{gamePath});

        if (rows == 0) {
            // 如果游戏不在历史中，先插入一条记录
            values.put(COL_HISTORY_NAME, gamePath.substring(gamePath.lastIndexOf('/') + 1));
            values.put(COL_HISTORY_PATH, gamePath);
            values.put(COL_HISTORY_IS_URI, 0);
            values.put(COL_HISTORY_LAST_PLAYED, System.currentTimeMillis());
            values.put(COL_HISTORY_PLAY_COUNT, 0);
            db.insert(TABLE_HISTORY, null, values);
        }
    }

    // ========== 数据类 ==========

    public static class HistoryEntry {
        public long id;
        public String gameName;
        public String gamePath;
        public boolean isUri;
        public String coverPath;
        public long lastPlayed;
        public int playCount;
        public String configJson;
    }
}
