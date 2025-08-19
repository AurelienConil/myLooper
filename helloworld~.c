#include "m_pd.h"
#include <math.h>
#include <string.h>

static t_class *helloworld_tilde_class = NULL;

typedef struct _helloworld_tilde
{
  t_object x_obj;
  t_sample f; // dummy variable for signal inlet

  // DSP Parameters - Add new parameters here
  t_float tremolo_freq;  // tremolo frequency in Hz
  t_float tremolo_phase; // current phase of tremolo oscillator
  // t_float gain;        // Example: volume gain (0.0 - 2.0)
  // t_float phase_offset; // Example: phase offset in degrees (0.0 - 360.0)

  // Internal state
  t_float sample_rate; // current sample rate

  // Outlets
  t_outlet *x_out_signal; // signal outlet
  t_outlet *x_out_info;   // info outlet
} t_helloworld_tilde;

// DSP perform routine
static t_int *helloworld_tilde_perform(t_int *w)
{
  t_helloworld_tilde *x = (t_helloworld_tilde *)(w[1]);
  t_sample *in = (t_sample *)(w[2]);
  t_sample *out = (t_sample *)(w[3]);
  int n = (int)(w[4]);

  t_float freq = x->tremolo_freq;
  t_float phase = x->tremolo_phase;
  t_float sr = x->sample_rate;

  // Safety check: if sample rate not set yet, pass through unchanged
  if (sr <= 0.0)
  {
    while (n--)
    {
      *out++ = *in++;
    }
    return (w + 5);
  }

  t_float phase_inc = 2.0 * M_PI * freq / sr;

  while (n--)
  {
    // Generate tremolo oscillator (sine wave between 0 and 1)
    t_float tremolo = 0.5 + 0.5 * sin(phase);

    // Apply tremolo to input signal
    *out++ = (*in++) * tremolo;

    // Update phase
    phase += phase_inc;
    if (phase >= 2.0 * M_PI)
    {
      phase -= 2.0 * M_PI;
    }
  }

  // Store updated phase
  x->tremolo_phase = phase;

  return (w + 5);
}

// DSP method
static void helloworld_tilde_dsp(t_helloworld_tilde *x, t_signal **sp)
{
  // Get the current sample rate from Pure Data's audio engine
  x->sample_rate = sp[0]->s_sr;

  // Add our perform routine to the DSP chain
  dsp_add(helloworld_tilde_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

// Parameter validation and setting utilities
typedef struct _param_desc
{
  const char *name;
  t_float min_val;
  t_float max_val;
  t_float *target;
} t_param_desc;

// Parameter setter function pointer
typedef void (*t_param_setter)(t_helloworld_tilde *x, const char *param_name, t_float value);

// Generic parameter setter with validation
static int set_parameter_float(t_helloworld_tilde *x, const char *param_name,
                               t_float value, t_float min_val, t_float max_val,
                               t_float *target)
{
  if (value >= min_val && value <= max_val)
  {
    *target = value;
    post("helloworld~: %s set to %.2f", param_name, value);
    return 1; // success
  }
  else
  {
    pd_error(x, "helloworld~: %s must be between %.2f and %.2f",
             param_name, min_val, max_val);
    return 0; // failure
  }
}

// Tremolo parameter setter
static void set_tremolo_freq(t_helloworld_tilde *x, const char *param_name, t_float value)
{
  set_parameter_float(x, param_name, value, 0.0, 100.0, &x->tremolo_freq);
}

// Parameter lookup table
static void handle_parameter_set(t_helloworld_tilde *x, t_symbol *param_sym, t_float value)
{
  const char *param_name = param_sym->s_name;

  // Parameter dispatch table
  if (strcmp(param_name, "tremolo") == 0)
  {
    set_tremolo_freq(x, param_name, value);
  }
  // TEMPLATE: How to add new parameters easily:
  // 1. Add variable to struct (e.g., t_float gain;)
  // 2. Initialize in constructor (e.g., x->gain = 1.0;)
  // 3. Add setter function (optional, or use set_parameter_float directly)
  // 4. Add dispatch case here:
  //
  // else if (strcmp(param_name, "gain") == 0) {
  //     set_parameter_float(x, param_name, value, 0.0, 2.0, &x->gain);
  // }
  // else if (strcmp(param_name, "phase") == 0) {
  //     set_parameter_float(x, param_name, value, 0.0, 360.0, &x->phase_offset);
  // }
  // 5. Add info output in helloworld_tilde_info()
  // 6. Use in perform routine
  else
  {
    pd_error(x, "helloworld~: unknown parameter '%s'", param_name);
  }
}

// Parse and validate message arguments
static int parse_set_message(t_helloworld_tilde *x, int argc, t_atom *argv,
                             t_symbol **param_out, t_float *value_out)
{
  if (argc < 2)
  {
    pd_error(x, "helloworld~: usage: set <parameter> <value>");
    return 0;
  }

  if (argv[0].a_type != A_SYMBOL)
  {
    pd_error(x, "helloworld~: parameter name must be a symbol");
    return 0;
  }

  if (argv[1].a_type != A_FLOAT)
  {
    pd_error(x, "helloworld~: parameter value must be a number");
    return 0;
  }

  *param_out = atom_getsymbol(&argv[0]);
  *value_out = atom_getfloat(&argv[1]);
  return 1;
}

// Simplified set method
static void helloworld_tilde_set(t_helloworld_tilde *x, t_symbol *s, int argc, t_atom *argv)
{
  (void)s; // silence unused parameter warning

  t_symbol *param;
  t_float value;

  if (parse_set_message(x, argc, argv, &param, &value))
  {
    handle_parameter_set(x, param, value);
  }
}

// Parameter info utilities
static void send_parameter_info(t_helloworld_tilde *x, const char *param_name, t_float value)
{
  t_atom info_list[2];
  SETSYMBOL(&info_list[0], gensym(param_name));
  SETFLOAT(&info_list[1], value);
  outlet_list(x->x_out_info, &s_list, 2, info_list);
}

// Extensible info method
static void helloworld_tilde_info(t_helloworld_tilde *x)
{
  // Send all parameter info
  send_parameter_info(x, "tremolo", x->tremolo_freq);

  // Add more parameters here easily:
  // send_parameter_info(x, "gain", x->gain);
  // send_parameter_info(x, "phase", x->phase_offset);

  // Also send general info
  t_atom info_list[2];
  SETSYMBOL(&info_list[0], gensym("sample_rate"));
  SETFLOAT(&info_list[1], x->sample_rate);
  outlet_list(x->x_out_info, &s_list, 2, info_list);
}

// Constructor
static void *helloworld_tilde_new(void)
{
  t_helloworld_tilde *x = (t_helloworld_tilde *)pd_new(helloworld_tilde_class);

  // Initialize parameters
  x->tremolo_freq = 2.0;  // default 2 Hz tremolo
  x->tremolo_phase = 0.0; // start at zero phase
  x->sample_rate = 0.0;   // will be set by DSP method when activated

  // Create second inlet for messages (accepts any message type)
  inlet_new(&x->x_obj, &x->x_obj.ob_pd, 0, 0);

  // Create signal outlet
  x->x_out_signal = outlet_new(&x->x_obj, &s_signal);

  // Create info outlet
  x->x_out_info = outlet_new(&x->x_obj, &s_anything);

  return (void *)x;
}

// Setup function
void helloworld_tilde_setup(void)
{
  helloworld_tilde_class = class_new(gensym("helloworld~"),
                                     (t_newmethod)helloworld_tilde_new,
                                     NULL,
                                     sizeof(t_helloworld_tilde),
                                     CLASS_DEFAULT,
                                     0);

  // Add DSP method
  class_addmethod(helloworld_tilde_class, (t_method)helloworld_tilde_dsp,
                  gensym("dsp"), A_CANT, 0);

  // Add message methods (these will work on any inlet)
  class_addmethod(helloworld_tilde_class, (t_method)helloworld_tilde_set,
                  gensym("set"), A_GIMME, 0);

  class_addmethod(helloworld_tilde_class, (t_method)helloworld_tilde_info,
                  gensym("info"), 0);

  // This is required for signal objects
  CLASS_MAINSIGNALIN(helloworld_tilde_class, t_helloworld_tilde, f);
}
