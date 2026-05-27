import { configureStore, createAsyncThunk, createReducer } from "@reduxjs/toolkit";
import { TypedUseSelectorHook, useDispatch, useSelector } from "react-redux";
import { Rpcs3Module, initRpcs3Module } from './Rpcs3Module';

export type EmuState = {
    value: string,
};

const initialState: EmuState = {
    value: "unknown",
};

export const init = createAsyncThunk<void>('init',
    async () => {
        await initRpcs3Module();
    }
);

// Boot routing. PS3 accepts:
//   .self / .elf        → raw PPU executable; written to MEMFS, BootElf
//   .iso / .bin / .pkg  → disc image / package; streamed via DiscImageDevice
//                         and started via BootDiscImage
export const bootFile = createAsyncThunk<void, File>('bootFile',
    async (file: File, thunkAPI) => {
        const fileName = file.name;
        const fileDotPos = fileName.lastIndexOf('.');
        if (fileDotPos === -1) {
            console.log("File name must have an extension.");
            thunkAPI.rejectWithValue(null);
            return;
        }
        const fileExtension = fileName.substring(fileDotPos).toLowerCase();
        if (fileExtension === ".elf" || fileExtension === ".self") {
            const url = URL.createObjectURL(file);
            const blob = await fetch(url).then(response => response.ok ? response.blob() : null);
            if (blob === null) {
                thunkAPI.rejectWithValue(null);
                return;
            }
            const data = new Uint8Array(await blob.arrayBuffer());
            const stream = Rpcs3Module.FS.open(fileName, "w+");
            Rpcs3Module.FS.write(stream, data, 0, data.length, 0);
            Rpcs3Module.FS.close(stream);
            URL.revokeObjectURL(url);
            Rpcs3Module.bootElf(fileName);
        } else {
            // .iso / .bin / .pkg are too large for MEMFS — stream via the
            // disc bridge instead. The C++ side calls back on demand.
            Rpcs3Module.discImageDevice.setFile(file);
            Rpcs3Module.bootDiscImage(fileName);
        }
    }
);

const reducer = createReducer(initialState, (builder) => (
    builder
        .addCase(init.fulfilled, (state) => {
            console.log("init");
            state.value = "initialized";
            return state;
        })
        .addCase(bootFile.fulfilled, (state) => {
            state.value = "loaded";
            return state;
        })
        .addCase(bootFile.rejected, (state, action) => {
            state.value = `loading failed: ${action.error.message}`;
            return state;
        })
));

export const store = configureStore({
    reducer: { rpcs3: reducer }
});

// Expose store + bootFile on window so an outer shell (Electron, mobile
// wrapper, integration tests) can drop in a file without the user picking
// it via the file input.
if (typeof window !== 'undefined') {
    (window as any).store = store;
    (window as any).bootFileAction = bootFile;
}

export type RootState = ReturnType<typeof store.getState>;
export type AppDispatch = typeof store.dispatch;

export const useAppDispatch = () => useDispatch<AppDispatch>();
export const useAppSelector: TypedUseSelectorHook<RootState> = useSelector;
