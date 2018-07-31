#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

enum Ports {
    PORT_IN,
    PORT_OUT,
    PORT_ARRAY_SIZE // this is not used as a port index
};

const size_t queue_size = 64*sizeof(jack_port_id_t);

typedef struct MIDI_MERGER_T {
  jack_client_t *client;
  jack_port_t *ports[PORT_ARRAY_SIZE];
  jack_ringbuffer_t *ports_to_connect;
} midi_merger_t;

/**
 * For use as a Jack-internal client, `jack_initialize()` and
 * `jack_finish()` have to be exported in the shared library.
 */
__attribute__ ((visibility("default")))
int jack_initialize(jack_client_t* client, const char* load_init);

__attribute__ ((visibility("default")))
void jack_finish(void* arg);


/**
 * Add a port_id to the waiting queue.
 */
size_t push_back(jack_ringbuffer_t *queue, jack_port_id_t port_id) {
  size_t written = 0;
  
  if (queue == NULL) {
    fprintf(stderr, "Could not schedule port connection.\n");
  } else {

    // Check if there is space to write
    size_t write_space = jack_ringbuffer_write_space(queue);
    if (write_space >= sizeof(jack_port_id_t)) {
      written = jack_ringbuffer_write(queue,
				      &port_id,
				      sizeof(jack_port_id_t));
      if (written < sizeof(jack_port_id_t)) {
	fprintf(stderr, "Could not schedule port connection.\n");
      }
    }    
  }
  return written;
}


/**
 * Return the next port_id or NULL if empty.
 */
jack_port_id_t next(jack_ringbuffer_t *queue) {
  size_t read = 0;
  jack_port_id_t id = NULL;
  
  if (queue == NULL) {
    fprintf(stderr, "Queue memory problem.\n");
  } else {
    char buffer[sizeof(jack_port_id_t)];
    read = jack_ringbuffer_read(queue, buffer, sizeof(jack_port_id_t));
    if (read == sizeof(jack_port_id_t)) {
      id = (jack_port_id_t) *buffer;      
    }
  }
  return id;
}


static int process_callback(jack_nframes_t nframes, void *arg)
{
  midi_merger_t *const mm = (midi_merger_t *const) arg;

  // Check if there are connections scheduled.
  jack_port_id_t source = next(mm->ports_to_connect);

  if (source != NULL) {
    int result;
    result = jack_connect(mm->client,
			  jack_port_name(jack_port_by_id(mm->client, source)),
			  jack_port_name(mm->ports[PORT_IN]));
    switch(result) {
    case 0:
      // Fine.
      break;
    case EEXIST:
      fprintf(stderr, "Connection exists.\n");
      break;
    default:
      fprintf(stderr, "Could not connect port.\n");
      break;
    }
  }

  // Get and clean the output buffer once per cycle.
  void *output_port_buffer = jack_port_get_buffer(mm->ports[PORT_OUT], nframes);
  jack_midi_clear_buffer(output_port_buffer);

  // Copy events from the input to the output.
  void *input_port_buffer = jack_port_get_buffer(mm->ports[PORT_IN], nframes);
  jack_nframes_t event_count = jack_midi_get_event_count(input_port_buffer); 
  if (event_count > 0) {
    
    jack_midi_event_t in_event;
    for (jack_nframes_t i = 0; i < event_count; ++i) {
      jack_midi_event_get(&in_event, input_port_buffer, i);
      
      int result;
      result = jack_midi_event_write(output_port_buffer,
				     in_event.time, in_event.buffer, in_event.size);
      switch(result) {
      case 0:
  	// Fine.
  	break;
      case ENOBUFS:
  	fprintf(stderr, "Not enough space for MIDI event.\n");
  	// Fall through
      default:
  	fprintf(stderr, "Could not write MIDI event.\n");
  	break;
      }      
    }
  }

  return 0;
}


static void port_registration_callback(jack_port_id_t port_id, int is_registered, void *arg)
{
  midi_merger_t *const mm = (midi_merger_t *const) arg;

  // If there is a new port of type MIDI output we connect it to our
  // input port.
  if (is_registered) {
    jack_port_t *source = jack_port_by_id(mm->client, port_id);
    
    // Check if MIDI output
    if (jack_port_flags(source) & JackPortIsOutput) {
      if (strcmp(jack_port_type(source), JACK_DEFAULT_MIDI_TYPE) == 0) {
	
	// Don't connect a loop to our own port
	if (source != mm->ports[PORT_OUT]) {
	  // We can't call jack_connect here in this
	  // callback. Schedule the connection for later.
	  push_back(mm->ports_to_connect, port_id);
	}
      }
    }
  }
  return;
}


int jack_initialize(jack_client_t* client, const char* load_init)
{
  midi_merger_t *const mm = malloc(sizeof(midi_merger_t));
  if (!mm) {
    fprintf(stderr, "Out of memory\n");
    return EXIT_FAILURE;
  }

  mm->client = client;

  // Register ports. We fake a physical output port.
  mm->ports[PORT_IN] = jack_port_register(client, "in",
					  JACK_DEFAULT_MIDI_TYPE,
					  JackPortIsInput, 0);
  mm->ports[PORT_OUT] = jack_port_register(client, "out",
					   JACK_DEFAULT_MIDI_TYPE,
					   JackPortIsOutput | JackPortIsPhysical, 0);
  for (int i = 0; i < PORT_ARRAY_SIZE; ++i) {
    if (!mm->ports[i]) {
      fprintf(stderr, "Can't register jack port\n");
      free(mm);
      return EXIT_FAILURE;
    }
  }

  // Create the ringbuffer (single-producer/single-consumer) for
  // scheduled port connections. It contains elements of type
  // `jack_port_id_t`.
  mm->ports_to_connect = jack_ringbuffer_create(queue_size);
  
  // Set callbacks
  jack_set_process_callback(client, process_callback, mm);
  jack_set_port_registration_callback(client, port_registration_callback, mm);

  /* Activate the jack client */
  if (jack_activate(client) != 0) {
    fprintf(stderr, "can't activate jack client\n");
    free(mm);
    return EXIT_FAILURE;
  }

  // TODO: Schedule already existing ports for connection.
  
  return 0;
}


void jack_finish(void* arg)
{
  midi_merger_t *const mm = (midi_merger_t *const) arg;

  jack_deactivate(mm->client);
  
  for (int i = 0; i < PORT_ARRAY_SIZE; ++i) {
    jack_port_unregister(mm->client, mm->ports[i]);
  }

  jack_ringbuffer_free(mm->ports_to_connect);
  
  free(mm);
}


/**
 * For testing purposes.
 */
int main() {
  int result = EXIT_FAILURE;
  jack_options_t options = JackNoStartServer;
  jack_status_t status;
  jack_client_t *client = jack_client_open("midi-merger", options, &status);
  if (client == NULL) {
    fprintf(stderr, "Opening client failed. Status is %d.\n", status);
    if (status & JackServerFailed) {
      fprintf(stderr, "Unable to connect to Jack server.\n");
    }
    return EXIT_FAILURE;
  }
  
  result = jack_initialize(client, "");

  while (1) {}
  
  jack_finish(client);
  
  return result;
}
