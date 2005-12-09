/*****************************************************************************
 * interaction.c: User interaction functions
 *****************************************************************************
 * Copyright (C) 1998-2004 VideoLAN
 * $Id: interface.c 10147 2005-03-05 17:18:30Z gbazin $
 *
 * Authors: Cl�ment Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/**
 *   \file
 *   This file contains functions related to user interaction management
 */

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* free(), strtol() */
#include <stdio.h>                                                   /* FILE */
#include <string.h>                                            /* strerror() */

#include <vlc/vlc.h>
#include <vlc/input.h>

#include "vlc_interaction.h"
#include "vlc_interface.h"
#include "vlc_playlist.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void                  intf_InteractionInit( playlist_t *p_playlist );
static interaction_t *       intf_InteractionGet( vlc_object_t *p_this );
static void                  intf_InteractionSearchInterface( interaction_t *
                                                          p_interaction );
static int                   intf_WaitAnswer( interaction_t *p_interact,
                             interaction_dialog_t *p_dialog );
static int                   intf_Send( interaction_t *p_interact,
                             interaction_dialog_t *p_dialog );
static interaction_dialog_t *intf_InteractionGetById( vlc_object_t* , int );

/**
 * Send an interaction element to the user
 *
 * \param p_this the calling vlc_object_t
 * \param p_interact the interaction element
 * \return VLC_SUCCESS or an error code
 */
int  __intf_Interact( vlc_object_t *p_this, interaction_dialog_t *
                                    p_dialog )
{

    interaction_t *p_interaction = intf_InteractionGet( p_this );

    /* Get an id, if we don't already have one */
    if( p_dialog->i_id == 0 )
    {
        p_dialog->i_id = ++p_interaction->i_last_id;
    }

    if( p_dialog->i_type == INTERACT_ASK )
    {
        return intf_WaitAnswer( p_interaction, p_dialog );
    }
    else
    {
        return intf_Send( p_interaction, p_dialog );
    }
}

/**
 * Destroy the interaction system
 */
void intf_InteractionDestroy( interaction_t *p_interaction )
{
    /// \todo Code this, and call it
}

/**
 * The main interaction processing loop
 * This function is called from the playlist loop
 *
 * \param p_playlist the parent playlist
 * \return nothing
 */
void intf_InteractionManage( playlist_t *p_playlist )
{
    vlc_value_t val;
    int i_index;
    interaction_t *p_interaction;

    p_interaction = p_playlist->p_interaction;

    // Nothing to do
    if( p_interaction->i_dialogs == 0 ) return;

    vlc_mutex_lock( &p_interaction->object_lock );

    intf_InteractionSearchInterface( p_interaction );

    if( !p_interaction->p_intf )
    {
        vlc_mutex_unlock( &p_interaction->object_lock );

        /// \todo Remove all dialogs as we can't display them
        return;
    }

    vlc_object_yield( p_interaction->p_intf );

    for( i_index = 0 ; i_index < p_interaction->i_dialogs; i_index ++ )
    {
        interaction_dialog_t *p_dialog = p_interaction->pp_dialogs[i_index];

        switch( p_dialog->i_status )
        {
        case ANSWERED_DIALOG:
            /// \todo Signal we have an answer
            // - If have answer, signal what is waiting
            // (vlc_cond ? dangerous in case of pb ?)

            // Ask interface to hide it
            msg_Dbg( p_interaction, "Hiding dialog %i", p_dialog->i_id );
            p_dialog->i_action = INTERACT_HIDE;
            val.p_address = p_dialog;
            var_Set( p_interaction->p_intf, "interaction", val );
            p_dialog->i_status = HIDING_DIALOG;
            break;
        case UPDATED_DIALOG:
            p_dialog->i_action = INTERACT_UPDATE;
            val.p_address = p_dialog;
            var_Set( p_interaction->p_intf, "interaction", val );
            p_dialog->i_status = SENT_DIALOG;
            msg_Dbg( p_interaction, "Updating dialog %i, %i widgets",
                                    p_dialog->i_id, p_dialog->i_widgets );
            break;
        case HIDDEN_DIALOG:
            if( !p_dialog->b_reusable )
            {
                /// \todo Destroy the dialog
            }
            break;
        case NEW_DIALOG:
            // This is truly a new dialog, send it.
            p_dialog->i_action = INTERACT_NEW;
            val.p_address = p_dialog;
            var_Set( p_interaction->p_intf, "interaction", val );
            msg_Dbg( p_interaction, "Creating dialog %i to interface %i, %i widgets",
                                        p_dialog->i_id, p_interaction->p_intf->i_object_id, p_dialog->i_widgets );
            p_dialog->i_status = SENT_DIALOG;
            break;
        }
    }

    vlc_object_release( p_interaction->p_intf );

    vlc_mutex_unlock( &p_playlist->p_interaction->object_lock );
}



#define INTERACT_INIT( new )                                            \
        new = (interaction_dialog_t*)malloc(                            \
                        sizeof( interaction_dialog_t ) );               \
        new->i_widgets = 0;                                             \
        new->pp_widgets = NULL;                                         \
        new->psz_title = NULL;                                          \
        new->psz_description = NULL;                                    \
        new->i_id = 0;                                                  \
        new->i_status = NEW_DIALOG;

#define INTERACT_FREE( new )                                            \
        if( new->psz_title ) free( new->psz_title );                    \
        if( new->psz_description ) free( new->psz_description );

/** Helper function to send a fatal message
 *  \param p_this     Parent vlc_object
 *  \param i_id       A predefined ID, 0 if not applicable
 *  \param psz_title  Title for the dialog
 *  \param psz_format The message to display
 *  */
void __intf_UserFatal( vlc_object_t *p_this, int i_id,
                       const char *psz_title,
                       const char *psz_format, ... )
{
    va_list args;
    interaction_dialog_t *p_new = NULL;
    user_widget_t *p_widget = NULL;

    if( i_id > 0 )
    {
        p_new = intf_InteractionGetById( p_this, i_id );
    }
    if( !p_new )
    {
        INTERACT_INIT( p_new );
        if( i_id > 0 ) p_new->i_id = i_id ;
    }
    else
    {
        p_new->i_status = UPDATED_DIALOG;
    }

    p_new->i_type = INTERACT_FATAL;
    p_new->psz_title = strdup( psz_title );

    p_widget = (user_widget_t* )malloc( sizeof( user_widget_t ) );

    p_widget->i_type = WIDGET_TEXT;

    va_start( args, psz_format );
    vasprintf( &p_widget->psz_text, psz_format, args );
    va_end( args );

    INSERT_ELEM ( p_new->pp_widgets,
                  p_new->i_widgets,
                  p_new->i_widgets,
                  p_widget );

    intf_Interact( p_this, p_new );
}

#if 0
/** Helper function to build a progress bar
 * \param p_this   Parent vlc object
 */
interaction_dialog_t *__intf_ProgressBuild( vlc_object_t *p_this,
                                            const char *psz_text )
{
    interaction_dialog_t *p_new = (interaction_dialog_t *)malloc(
                                        sizeof( interaction_dialog_t ) );


    return p_new;
}
#endif



/**********************************************************************
 * The following functions are local
 **********************************************************************/

/* Get the interaction object. Create it if needed */
static interaction_t * intf_InteractionGet( vlc_object_t *p_this )
{
    playlist_t *p_playlist;
    interaction_t *p_interaction;

    p_playlist = (playlist_t*) vlc_object_find( p_this, VLC_OBJECT_PLAYLIST,
                                                FIND_ANYWHERE );

    if( !p_playlist )
    {
        return NULL;
    }

    if( p_playlist->p_interaction == NULL )
    {
        intf_InteractionInit( p_playlist );
    }

    p_interaction = p_playlist->p_interaction;

    vlc_object_release( p_playlist );

    return p_interaction;
}

/* Create the interaction object in the given playlist object */
static void intf_InteractionInit( playlist_t *p_playlist )
{
    interaction_t *p_interaction;

    msg_Dbg( p_playlist, "initializing interaction system" );

    p_interaction = vlc_object_create( VLC_OBJECT( p_playlist ),
                                       sizeof( interaction_t ) );
    if( !p_interaction )
    {
        msg_Err( p_playlist,"out of memory" );
        return;
    }

    p_interaction->i_dialogs = 0;
    p_interaction->pp_dialogs = NULL;
    p_interaction->p_intf = NULL;
    p_interaction->i_last_id = DIALOG_LAST_PREDEFINED + 1;

    vlc_mutex_init( p_interaction , &p_interaction->object_lock );

    p_playlist->p_interaction  = p_interaction;
}

/* Look for an interface suitable for interaction */
static void intf_InteractionSearchInterface( interaction_t *p_interaction )
{
    vlc_list_t  *p_list;
    int          i_index;

    p_interaction->p_intf = NULL;

    p_list = vlc_list_find( p_interaction, VLC_OBJECT_INTF, FIND_ANYWHERE );
    if( !p_list )
    {
        msg_Err( p_interaction, "Unable to create module list" );
        return;
    }

    for( i_index = 0; i_index < p_list->i_count; i_index ++ )
    {
        intf_thread_t *p_intf = (intf_thread_t *)
                                        p_list->p_values[i_index].p_object;
        if( p_intf->b_interaction )
        {
            p_interaction->p_intf = p_intf;
            break;
        }
    }
    vlc_list_release ( p_list );
}

/* Add a dialog to the queue and wait for answer */
static int intf_WaitAnswer( interaction_t *p_interact, interaction_dialog_t *p_dialog )
{
    // TODO: Add to queue, wait for answer
    return VLC_SUCCESS;
}

/* Add a dialog to the queue and return */
static int intf_Send( interaction_t *p_interact, interaction_dialog_t *p_dialog )
{
    vlc_mutex_lock( &p_interact->object_lock );

    /// \todo Check first it does not exist !!!
    INSERT_ELEM( p_interact->pp_dialogs,
                 p_interact->i_dialogs,
                 p_interact->i_dialogs,
                 p_dialog );
    vlc_mutex_unlock( &p_interact->object_lock );
    return VLC_SUCCESS;
}

/* Find an interaction dialog by its id */
static interaction_dialog_t *intf_InteractionGetById( vlc_object_t* p_this,
                                                       int i_id )
{
    interaction_t *p_interaction = intf_InteractionGet( p_this );
    int i;

    for( i = 0 ; i< p_interaction->i_dialogs; i++ )
    {
        if( p_interaction->pp_dialogs[i]->i_id == i_id )
        {
            return p_interaction->pp_dialogs[i];
        }
    }
    return NULL;
}
