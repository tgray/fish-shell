/** \file event.c

	Functions for handling event triggers

*/
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
#include <algorithm>

#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "function.h"
#include "proc.h"
#include "parser.h"
#include "common.h"
#include "event.h"
#include "signal.h"

/**
   Number of signals that can be queued before an overflow occurs
*/
#define SIG_UNHANDLED_MAX 64

/**
   This struct contains a list of generated signals waiting to be
   dispatched
*/
typedef struct
{
	/**
	   Number of delivered signals
	*/
	int count;
	/**
	   Whether signals have been skipped
	*/
	int overflow;	
	/**
	   Array of signal events
	*/
	int signal[SIG_UNHANDLED_MAX];	
}
	signal_list_t;

/**
  The signal event list. Actually two separate lists. One which is
  active, which is the one that new events is written to. The inactive
  one contains the events that are currently beeing performed.
*/
static signal_list_t sig_list[]={{0,0},{0,0}};

/**
   The index of sig_list that is the list of signals currently written to
*/
static int active_list=0;

typedef std::vector<event_t *> event_list_t; 

/**
   List of event handlers.
   Note this is inspected by our signal handler, so we must block signals around manipulating it.
*/
static event_list_t events;
/**
   List of event handlers that should be removed
*/
static event_list_t killme;

/**
   List of events that have been sent but have not yet been delivered because they are blocked.
*/
static event_list_t blocked;

/**
   Tests if one event instance matches the definition of a event
   class. If both the class and the instance name a function,
   they must name the same function.

*/
static int event_match( const event_t *classv, const event_t *instance )
{

    /* If the function names are both non-empty and different, then it's not a match */
	if( ! classv->function_name.empty() &&
        ! instance->function_name.empty() &&
          classv->function_name != instance->function_name)
	{
        return 0;
	}

	if( classv->type == EVENT_ANY )
		return 1;
	
	if( classv->type != instance->type )
		return 0;
	

	switch( classv->type )
	{
		
		case EVENT_SIGNAL:
			if( classv->param1.signal == EVENT_ANY_SIGNAL )
				return 1;
			return classv->param1.signal == instance->param1.signal;
			
		case EVENT_VARIABLE:
			return instance->str_param1 == classv->str_param1;
			
		case EVENT_EXIT:
			if( classv->param1.pid == EVENT_ANY_PID )
				return 1;
			return classv->param1.pid == instance->param1.pid;

		case EVENT_JOB_ID:
			return classv->param1.job_id == instance->param1.job_id;

		case EVENT_GENERIC:
			return instance->str_param1 == classv->str_param1;

	}
	
	/**
	   This should never be reached
	*/
	return 0;	
}


/**
   Create an identical copy of an event. Use deep copying, i.e. make
   duplicates of any strings used as well.
*/
static event_t *event_copy( const event_t *event, int copy_arguments )
{
    event_t *e = new event_t(*event);
    
    e->arguments.reset(new wcstring_list_t);
	if( copy_arguments && event->arguments.get() != NULL )
	{
        *(e->arguments) = *(event->arguments);				
	}
	
	return e;
}

/**
   Test if specified event is blocked
*/
static int event_is_blocked( event_t *e )
{
	block_t *block;
	parser_t &parser = parser_t::principal_parser();
	for( block = parser.current_block; block; block = block->outer )
	{
        if (event_block_list_blocks_type(block->event_blocks, e->type))
            return true;
	}
    return event_block_list_blocks_type(parser.global_event_blocks, e->type);
}

wcstring event_get_desc( const event_t *e )
{

	CHECK( e, 0 );

	wcstring result;	
	switch( e->type )
	{
		
		case EVENT_SIGNAL:
            result = format_string(_(L"signal handler for %ls (%ls)"), sig2wcs(e->param1.signal ), signal_get_desc( e->param1.signal ));
			break;
		
		case EVENT_VARIABLE:
			result = format_string(_(L"handler for variable '%ls'"), e->str_param1.c_str() );
			break;
		
		case EVENT_EXIT:
			if( e->param1.pid > 0 )
			{
				result = format_string(_(L"exit handler for process %d"), e->param1.pid );
			}
			else
			{
				job_t *j = job_get_from_pid( -e->param1.pid );
				if( j )
					result = format_string(_(L"exit handler for job %d, '%ls'"), j->job_id, j->command_wcstr() );
				else
					result = format_string(_(L"exit handler for job with process group %d"), -e->param1.pid );
			}
			
			break;
		
		case EVENT_JOB_ID:
		{
			job_t *j = job_get( e->param1.job_id );
			if( j )
				result = format_string(_(L"exit handler for job %d, '%ls'"), j->job_id, j->command_wcstr() );
			else
				result = format_string(_(L"exit handler for job with job id %d"), e->param1.job_id );

			break;
		}
		
		case EVENT_GENERIC:
			result = format_string(_(L"handler for generic event '%ls'"), e->str_param1.c_str() );
			break;
			
		default:
			result = format_string(_(L"Unknown event type") );
			break;
			
	}
	
	return result;
}

#if 0
static void show_all_handlers(void) {
    puts("event handlers:");
    for (event_list_t::const_iterator iter = events.begin(); iter != events.end(); ++iter) {
        const event_t *foo = *iter;
        wcstring tmp = event_get_desc(foo);
        printf("    handler now %ls\n", tmp.c_str());
    }
}
#endif

void event_add_handler( const event_t *event )
{
	event_t *e;

	CHECK( event, );
	
	e = event_copy( event, 0 );

	if( e->type == EVENT_SIGNAL )
	{
		signal_handle( e->param1.signal, 1 );
	}
    
    // Block around updating the events vector
    signal_block();
    events.push_back(e);
    signal_unblock();
}

void event_remove( event_t *criterion )
{

	size_t i;
	event_list_t new_list;
	
	CHECK( criterion, );

	/*
	  Because of concurrency issues (env_remove could remove an event
	  that is currently being executed), env_remove does not actually
	  free any events - instead it simply moves all events that should
	  be removed from the event list to the killme list, and the ones
	  that shouldn't be killed to new_list, and then drops the empty
	  events-list.
	*/
	
	if( events.empty() )
		return;

	for( i=0; i<events.size(); i++ )
	{
		event_t *n = events.at(i);
		if( event_match( criterion, n ) )
		{
            killme.push_back(n);

			/*
			  If this event was a signal handler and no other handler handles
			  the specified signal type, do not handle that type of signal any
			  more.
			*/
			if( n->type == EVENT_SIGNAL )
			{
                event_t e = event_t::signal_event(n->param1.signal);
				if( event_get( &e, 0 ) == 1 )
				{
					signal_handle( e.param1.signal, 0 );
				}		
			}
		}
		else
		{
            new_list.push_back(n);
		}
	}
    signal_block();
	events.swap(new_list);
    signal_unblock();
}

int event_get( event_t *criterion, std::vector<event_t *> *out )
{
	size_t i;
	int found = 0;
	
	if( events.empty() )
		return 0;	

	CHECK( criterion, 0 );
	
	for( i=0; i<events.size(); i++ )
	{
		event_t *n = events.at(i);
		if( event_match(criterion, n ) )
		{
			found++;
			if( out )
                out->push_back(n);
		}		
	}
	return found;
}

bool event_is_signal_observed(int sig)
{
    /* We are in a signal handler! Don't allocate memory, etc.
       This does what event_match does, except it doesn't require passing in an event_t.
    */
    size_t i, max = events.size();
    for (i=0; i < max; i++)
    {
        const event_t *event = events[i];
        if (event->type == EVENT_ANY)
        {
            return true;
        }
        else if (event->type == EVENT_SIGNAL)
        {
            if( event->param1.signal == EVENT_ANY_SIGNAL || event->param1.signal == sig)
                return true;
        }
    }
    return false;
}

/**
   Free all events in the kill list
*/
static void event_free_kills()
{
    for_each(killme.begin(), killme.end(), event_free);
    killme.resize(0);
}

/**
   Test if the specified event is waiting to be killed
*/
static int event_is_killed( event_t *e )
{
    return std::find(killme.begin(), killme.end(), e) != killme.end();
}	

/**
   Perform the specified event. Since almost all event firings will
   not be matched by even a single event handler, we make sure to
   optimize the 'no matches' path. This means that nothing is
   allocated/initialized unless needed.
*/
static void event_fire_internal( const event_t *event )
{

	size_t i, j;
	event_list_t fire;
	
	/*
	  First we free all events that have been removed
	*/
	event_free_kills();	

	if( events.empty() )
		return;

	/*
	  Then we iterate over all events, adding events that should be
	  fired to a second list. We need to do this in a separate step
	  since an event handler might call event_remove or
	  event_add_handler, which will change the contents of the \c
	  events list.
	*/
	for( i=0; i<events.size(); i++ )
	{
		event_t *criterion = events.at(i);
		
		/*
		  Check if this event is a match
		*/
		if(event_match( criterion, event ) )
		{
            fire.push_back(criterion);
		}
	}
	
	/*
	  No matches. Time to return.
	*/
	if( fire.empty() )
		return;
		
	/*
	  Iterate over our list of matching events
	*/
	
	for( i=0; i<fire.size(); i++ )
	{
		event_t *criterion = fire.at(i);
		int prev_status;

		/*
		  Check if this event has been removed, if so, dont fire it
		*/
		if( event_is_killed( criterion ) )
			continue;

		/*
		  Fire event
		*/
		wcstring buffer = criterion->function_name;
		
        if (event->arguments.get())
        {
            for( j=0; j< event->arguments->size(); j++ )
            {
                wcstring arg_esc = escape_string( event->arguments->at(j), 1 );		
                buffer += L" ";
                buffer += arg_esc;
            }
        }

//		debug( 1, L"Event handler fires command '%ls'", (wchar_t *)b->buff );
		
		/*
		  Event handlers are not part of the main flow of code, so
		  they are marked as non-interactive
		*/
		proc_push_interactive(0);
		prev_status = proc_get_last_status();
        parser_t &parser = parser_t::principal_parser();
		parser.push_block( EVENT );
		parser.current_block->state1<const event_t *>() = event;
		parser.eval( buffer.c_str(), 0, TOP );
		parser.pop_block();
		proc_pop_interactive();					
		proc_set_last_status( prev_status );
	}

	/*
	  Free killed events
	*/
	event_free_kills();	
	
}

/**
   Handle all pending signal events
*/
static void event_fire_delayed()
{

	size_t i;

	/*
	  If is_event is one, we are running the event-handler non-recursively. 

	  When the event handler has called a piece of code that triggers
	  another event, we do not want to fire delayed events because of
	  concurrency problems.
	*/
	if( ! blocked.empty() && is_event==1)
	{
		event_list_t new_blocked;
		
		for( i=0; i<blocked.size(); i++ )
		{
			event_t *e = blocked.at(i);
			if( event_is_blocked( e ) )
			{
                new_blocked.push_back(e);
			}
			else
			{
				event_fire_internal( e );
				event_free( e );
			}
		}		
        blocked.swap(new_blocked);
	}
	
	while( sig_list[active_list].count > 0 )
	{
		signal_list_t *lst;

		/*
		  Switch signal lists
		*/
		sig_list[1-active_list].count=0;
		sig_list[1-active_list].overflow=0;
		active_list=1-active_list;

		/*
		  Set up 
		*/
        event_t e = event_t::signal_event(0);
        e.arguments.reset(new wcstring_list_t(1)); //one element
		lst = &sig_list[1-active_list];
		
		if( lst->overflow )
		{
			debug( 0, _( L"Signal list overflow. Signals have been ignored." ) );
		}
		
		/*
		  Send all signals in our private list
		*/
		for( i=0; i<(size_t)lst->count; i++ )
		{
			e.param1.signal = lst->signal[i];
            e.arguments->at(0) = sig2wcs( e.param1.signal );
			if( event_is_blocked( &e ) )
			{
                blocked.push_back(event_copy(&e, 1));
			}
			else
			{
				event_fire_internal( &e );
			}
		}
		
        e.arguments.reset(NULL);
		
	}	
}

void event_fire_signal(int signal)
{
    /*
      This means we are in a signal handler. We must be very
      careful not do do anything that could cause a memory
      allocation or something else that might be bad when in a
      signal handler.
    */
    if( sig_list[active_list].count < SIG_UNHANDLED_MAX )
        sig_list[active_list].signal[sig_list[active_list].count++]=signal;
    else
        sig_list[active_list].overflow=1;
}


void event_fire( event_t *event )
{
	
	if( event && (event->type == EVENT_SIGNAL) )
	{
        event_fire_signal(event->param1.signal);
	}
	else
	{
        is_event++;

		/*
		  Fire events triggered by signals
		*/
		event_fire_delayed();
			
		if( event )
		{
			if( event_is_blocked( event ) )
			{
                blocked.push_back(event_copy(event, 1));
			}
			else
			{
				event_fire_internal( event );
			}
		}
        is_event--;
	}	
}


void event_init()
{
}

void event_destroy()
{

    for_each(events.begin(), events.end(), event_free);
    events.clear();
    
    for_each(killme.begin(), killme.end(), event_free);
    killme.clear();
}

void event_free( event_t *e )
{
	CHECK( e, );
    delete e;
}


void event_fire_generic_internal(const wchar_t *name, ...)
{
	va_list va;
	wchar_t *arg;

	CHECK( name, );

	event_t ev(EVENT_GENERIC);
	ev.str_param1 = name;
    ev.arguments.reset(new wcstring_list_t);
	va_start( va, name );
	while( (arg=va_arg(va, wchar_t *) )!= 0 )
	{
        ev.arguments->push_back(arg);
	}
	va_end( va );
	
	event_fire( &ev );
    ev.arguments.reset(NULL);
}

event_t event_t::signal_event(int sig) {
    event_t event(EVENT_SIGNAL);
    event.param1.signal = sig;
    return event;
}

event_t event_t::variable_event(const wcstring &str) {
    event_t event(EVENT_VARIABLE);
    event.str_param1 = str;
    return event;
}

event_t event_t::generic_event(const wcstring &str) {
    event_t event(EVENT_GENERIC);
    event.str_param1 = str;
    return event;
}

event_t::event_t(const event_t &x) :
                type(x.type),
                param1(x.param1),
                str_param1(x.str_param1),
                function_name(x.function_name)
{
    const wcstring_list_t *ptr = x.arguments.get();
    if (ptr)
        arguments.reset(new wcstring_list_t(*ptr));
}
