# Pure Data DSP External Template

A comprehensive template for creating DSP externals in Pure Data using [pd-lib-builder](https://github.com/pure-data/pd-lib-builder). This template provides a solid foundation with best practices for building robust, extensible audio processing objects.

## Features

### 🎵 **DSP Processing**
- **Clean DSP architecture** with proper initialization/cleanup
- **Sample rate awareness** (automatically adapts to Pure Data's audio settings)
- **Phase management** without numerical overflow
- **Performance optimized** audio processing loop

### 🎛️ **Parameter Control**
- **Runtime parameter modification** via messages
- **Validation and error handling** for all parameters
- **Extensible parameter system** - easily add new parameters
- **Info system** to query current parameter values

### 🔌 **Pure Data Integration**
- **Signal inlet** for audio input
- **Message inlet** for parameter control
- **Signal outlet** for processed audio
- **Info outlet** for parameter feedback

## Example: Tremolo Effect

This template demonstrates a simple tremolo effect that modulates the input signal with a sine wave. It's designed as a **proof of concept** showing all the essential DSP external patterns.

### Usage in Pure Data:
```
[helloworld~]  // Create the object
|
[set tremolo 4<  // Set tremolo frequency to 4 Hz
[info<           // Get current parameter values
```

## Quick Start

### 1. Clone and Build
```bash
git clone --recursive https://github.com/AurelienConil/Pd-external-template.git
cd Pd-external-template
make
```

### 2. Test in Pure Data
- Copy `helloworld~.pd_darwin` to your Pure Data externals folder
- Open `helloworld~-help.pd` for usage examples
- Connect audio input and try the controls

## Building Your Own External

### 1. **Rename Everything**
```bash
# Rename files
mv helloworld~.c yourexternal~.c
mv helloworld~-help.pd yourexternal~-help.pd
mv helloworld~-meta.pd yourexternal~-meta.pd

# Update Makefile
lib.name = yourexternal~
```

### 2. **Modify the DSP Processing**
Replace the tremolo code in `*_perform()` with your own DSP algorithm:

```c
// In yourexternal_tilde_perform()
while (n--) {
    // Your DSP processing here
    *out++ = your_process_function(*in++, your_parameters);
}
```

### 3. **Add New Parameters**
Follow the template pattern:

```c
// 1. Add to struct
typedef struct _yourexternal_tilde {
    // ... existing fields ...
    t_float your_new_param;  // Add your parameter
} t_yourexternal_tilde;

// 2. Initialize in constructor
x->your_new_param = default_value;

// 3. Add parameter handler
else if (strcmp(param_name, "yourparam") == 0) {
    set_parameter_float(x, param_name, value, min_val, max_val, &x->your_new_param);
}

// 4. Add to info output
send_parameter_info(x, "yourparam", x->your_new_param);

// 5. Use in DSP processing
// Use x->your_new_param in your perform routine
```

## Architecture Overview

### **Core Components:**

1. **DSP Chain Integration**
   - `*_perform()`: Audio processing routine (called for every audio block)
   - `*_dsp()`: DSP setup (called when audio starts/stops)

2. **Parameter Management**
   - `parse_set_message()`: Validates incoming messages
   - `handle_parameter_set()`: Routes parameters to appropriate handlers
   - `set_parameter_float()`: Generic parameter setter with validation

3. **Message Interface**
   - `set <param> <value>`: Set parameter values
   - `info`: Query current parameter state

### **Key Design Principles:**

- ✅ **No malloc/free in perform routine** (performance critical)
- ✅ **Parameter validation** prevents crashes and invalid states
- ✅ **Extensible architecture** for easy parameter addition
- ✅ **Proper DSP lifecycle management**
- ✅ **Sample rate adaptation** 

## Build Requirements

- **Pure Data source code** ([installation guide](https://puredata.info/docs/developer/GettingPdSource))
- **C compiler** (GCC, Clang, or MSVC)
- **Make** build system

### Platform-specific builds:
```bash
# macOS
make install pdincludepath=../pure-data/src/ objectsdir=./build

# Linux
make install pdincludepath=../pure-data/src/ objectsdir=./build

# Windows (with MSYS2/MinGW)
make install pdincludepath=../pure-data/src/ objectsdir=./build
```

## Distribution

### Using deken (recommended):
```bash
# Build for distribution
make install objectsdir=./build

# Upload to Pure Data library (requires puredata.info account)
deken upload ./build/yourexternal~
```

## Contributing

This template is designed to demonstrate best practices for Pure Data external development. Feel free to:

- **Report issues** if you find problems with the template
- **Suggest improvements** for better DSP patterns
- **Share your externals** built with this template

## License

This template follows Pure Data's licensing conventions. Adapt as needed for your specific external.

---

