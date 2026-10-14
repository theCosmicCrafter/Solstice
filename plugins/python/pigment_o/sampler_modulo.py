# Pigment.O is a Krita plugin and it is a Color Picker and Color Mixer and Color Sampler.
# Copyright ( C ) 2020  Ricardo Jeremias.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# ( at your option ) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


#region Imports

# Krita
from krita import *
# PyQt5
from PyQt5 import QtWidgets, QtCore, QtGui, uic
# Engine
from .engine_calculations import *

#endregion


#region Panels

class Display_Map( QWidget ):
    SIGNAL_INSERT = QtCore.pyqtSignal()
    SIGNAL_CLEAN = QtCore.pyqtSignal()

    # Init
    def __init__( self, parent ):
        super( Display_Map, self ).__init__( parent )
        self.Variables()
    def sizeHint( self ):
        return QtCore.QSize( render_width, render_height )
    # Variables
    def Variables( self ):
        # Widget
        self.ww = 1
        self.hh = 1
        self.w2 = 0.5
        self.h2 = 0.5
        # Event
        self.ox = 0
        self.oy = 0
        self.ex = 0
        self.ey = 0
        # State
        self.state_press = False
        self.operation = None
        # Display
        self.qpixmap_display = None
        self.scale_method = Qt.FastTransformation
        # Interaction
        self.operation = None
        # Camera
        self.pcmx = 0
        self.pcmy = 0
        self.pcz = 1
        self.cmx = 0 # Moxe X
        self.cmy = 0 # Move Y
        self.cz = 1 # Zoom
        self.display = False
        self.background = False # Background Color
        # Colors
        self.color_red = QColor( 84, 43, 43 )
    # Relay
    def Set_Size( self, ww, hh ):
        self.ww = int( ww )
        self.hh = int( hh )
        self.w2 = int( ww * 0.5 )
        self.h2 = int( hh * 0.5 )
        self.resize( ww, hh )
    # Update
    def Update_Display( self, qpixmap ):
        self.qpixmap_display = qpixmap
        self.update()
    def Update_Background( self, boolean ):
        self.background = boolean
        self.update()
    # Mouse Events
    def mousePressEvent( self, event ):
        # Variable
        self.state_press = True
        # Event
        em = event.modifiers()
        eb = event.buttons()
        ex = event.x()
        ey = event.y()
        self.ox = ex
        self.oy = ey
        self.ex = ex
        self.ey = ey
        self.operation = None
        # LMB
        if ( em == QtCore.Qt.ShiftModifier and eb == QtCore.Qt.LeftButton ):    self.Camera_Previous(); self.operation = "camera_move"
        if ( em == QtCore.Qt.ControlModifier and eb == QtCore.Qt.LeftButton ):  self.Camera_Reset()
        # MMB
        if ( em == QtCore.Qt.NoModifier and eb == QtCore.Qt.MiddleButton ):     self.Camera_Previous(); self.operation = "camera_move"
        # RMB
        if ( em == QtCore.Qt.NoModifier and eb == QtCore.Qt.RightButton ):      self.Context_Menu( event )
        if ( em == QtCore.Qt.ShiftModifier and eb == QtCore.Qt.RightButton ):   self.Camera_Previous(); self.operation = "camera_scale"
        if ( em == QtCore.Qt.ControlModifier and eb == QtCore.Qt.RightButton ): self.Camera_Reset()
        # Update
        self.update()
    def mouseMoveEvent( self, event ):
        # Event
        ex = event.x()
        ey = event.y()
        self.ex = ex
        self.ey = ey
        # Camera
        if self.operation == "camera_move":     self.Camera_Move( ex, ey )
        if self.operation == "camera_scale":    self.Camera_Scale( ex, ey )
        # Update
        self.update()
    def mouseDoubleClickEvent( self, event ):
        pass
    def mouseReleaseEvent( self, event ):
        # Variables
        self.state_press = False
        self.operation = None
        # Update
        self.update()
    # Context Menu
    def Context_Menu( self, event ):
        qmenu = QMenu( self )
        action_mask = qmenu.addAction( "Insert" )
        action_clean = qmenu.addAction( "Clean" )
        action = qmenu.exec_( self.mapToGlobal( event.pos() ) )
        if action == action_mask:
            self.SIGNAL_INSERT.emit()
        if action == action_clean:
            self.SIGNAL_CLEAN.emit()
    # Camera
    def Camera_Reset( self ):
        self.cmx = 0
        self.cmy = 0
        self.cz = 1
    def Camera_Previous( self ):
        self.pcmx = self.cmx
        self.pcmy = self.cmy
        self.pcz = self.cz
    def Camera_Move( self, ex, ey ):
        if self.cz != 0:
            self.cmx = self.pcmx + ( ( ex - self.ox ) / self.cz )
            self.cmy = self.pcmy + ( ( ey - self.oy ) / self.cz )
    def Camera_Scale( self, ex, ey ):
        factor = 200
        self.cz = Limit_Range( self.pcz - ( ( ey - self.oy ) / factor ), 0, 100 )
    # Painter
    def paintEvent( self, event ):
        # Painter
        painter = QPainter( self )
        painter.setRenderHint( QtGui.QPainter.Antialiasing, True )
        # Background Hover
        painter.setPen( QtCore.Qt.NoPen )
        if self.background == True:
            painter.setBrush( QBrush( self.color_red ) )
            painter.drawRect( 0, 0, self.ww, self.hh )
        # Mask
        painter.setClipRect( QRect( 0, 0, self.ww, self.hh ), Qt.ReplaceClip )
        # Render Image
        qpixmap = self.qpixmap_display
        render = True
        if qpixmap in [ None, False, True ]:
            render = False
        if render == True:
            # Draw Pixmap
            draw = self.Draw_Render( qpixmap )
            painter.drawPixmap( int( self.bl ), int( self.bt ), draw )
    def Draw_Render( self, qpixmap ):
        # QPixmap
        if self.display == False:
            draw = qpixmap.scaled( int( self.ww * self.cz ), int( self.hh * self.cz ), Qt.KeepAspectRatio, self.scale_method )
        else:
            ww = qpixmap.width()
            hh = qpixmap.height()
            draw = qpixmap.scaled( int( ww * self.cz ), int( hh * self.cz ), Qt.KeepAspectRatio, self.scale_method )
        self.bw = draw.width()
        self.bh = draw.height()
        # Variables
        self.bl = self.w2 - ( self.bw * 0.5 ) + ( self.cmx * self.cz )
        self.bt = self.h2 - ( self.bh * 0.5 ) + ( self.cmy * self.cz )
        self.br = self.bl + self.bw
        self.bb = self.bt + self.bh
        # Return
        return draw

class Channel_Select( QWidget ):
    SIGNAL_INDEX = QtCore.pyqtSignal( int )

    # Init
    def __init__( self, parent ):
        super( Channel_Select, self ).__init__( parent )
        self.Variables()
    def sizeHint( self ):
        return QtCore.QSize( render_width, 100 )
    # Variables
    def Variables( self ):
        # Widget
        self.ww = 1
        self.hh = 1
        self.w2 = 0.5
        self.h2 = 0.5
        self.start_x = 0
        # Event
        self.ex = 0
        self.ey = 0
        # Channels
        self.channel_number = 5
        self.channel_index = 0
        self.channel_map = list()
        # State
        self.state_press = False
        self.operation = None
        #Button
        self.bw = 20
        self.bh = 10
        self.bi = 6
        self.bb = self.bw + self.bi
        # Colors
        self.c_lite = QColor( "#ffffff" )
        self.c_dark = QColor( "#000000" )
        # Icons
        self.margin = 5
        self.total_width = 0
        self.item_width = 0
    # Relay
    def Set_Size( self, ww, hh ):
        self.ww = int( ww )
        self.hh = int( hh )
        self.w2 = int( ww * 0.5 )
        self.h2 = int( hh * 0.5 )
        self.Start_Pixel()
        self.resize( ww, hh )
    def Set_Theme( self, c_lite, c_dark ):
        self.c_lite = QColor( c_lite )
        self.c_dark = QColor( c_dark )
        self.c_dark.setAlpha( 200 )
    # Update
    def Update_Channel_Number( self, channel_number ):
        self.channel_number = channel_number
        self.update()
    def Update_Channel_Map( self, channel_map ):
        # Variables
        self.channel_map = channel_map
        # Sizes
        if self.channel_map != None:
            self.total_width = 0
            for item in channel_map:
                width = item["width"]
                self.total_width += width
            self.total_width += self.margin * ( len( channel_map ) - 1 )
            self.item_width = width
            self.Start_Pixel()
        # Update
        self.update()
    # Calculate
    def Start_Pixel( self ):
        self.start_x = int( self.w2 - ( self.total_width * 0.5 ) )
    # Mouse Events
    def mousePressEvent( self, event ):
        # Variable
        self.state_press = True
        # Event
        em = event.modifiers()
        eb = event.buttons()
        ex = event.x()
        ey = event.y()
        self.ex = ex
        self.ey = ey
        self.operation = None
        # LMB
        if ( em == QtCore.Qt.NoModifier and eb == QtCore.Qt.LeftButton ):
            self.operation = "channel_select"
            self.Channel_Select( ex, ey )
        # Update
        self.update()
    def mouseMoveEvent( self, event ):
        # Event
        ex = event.x()
        ey = event.y()
        self.ex = ex
        self.ey = ey
        # Operations
        if self.operation == "channel_select":
            self.Channel_Select( ex, ey )
        # Update
        self.update()
    def mouseReleaseEvent( self, event ):
        # Variables
        self.state_press = False
        self.operation = None
        # Update
        self.update()
    # Wheel Event
    def wheelEvent( self, event ):
        delta_y = event.angleDelta().y()
        angle = 5
        if delta_y >= +angle:   self.Channel_Wheel( +1 )
        if delta_y <= -angle:   self.Channel_Wheel( -1 )
    # Operation
    def Channel_Select( self, ex, ey ):
        if self.channel_map != None:
            for i in range( 0, len( self.channel_map ) ):
                px = self.start_x + ( self.item_width * i ) + ( self.margin * i )
                pw = px + self.item_width
                if ( ex >= px ) and ( ex <= pw ):
                    self.channel_index = i
                    self.Channel_Signal( self.channel_index )
                    break
        self.update()
    def Channel_Wheel( self, value ):
        # Variables
        index = self.channel_index + value
        if index <= 0:                          index = 0
        if index >= self.channel_number - 1:    index = self.channel_number - 1
        # Send
        self.channel_index = int( index )
        self.Channel_Signal( self.channel_index )
        self.update()
    def Channel_Signal( self, index ):
        self.SIGNAL_INDEX.emit( index )
    # Painter
    def paintEvent( self, event ):
        # Painter
        painter = QPainter( self )
        painter.setRenderHint( QtGui.QPainter.Antialiasing, True )
        # Paint Images
        if self.channel_map != None:
            for i in range( 0, len( self.channel_map ) ):
                # Variables
                item = self.channel_map[i]
                qpixmap = item["render"]
                width = item["width"]
                height = item["height"]
                text = item["text"]
                # Calculation
                px = self.start_x + ( width * i ) + ( self.margin * i )
                py = self.h2 - height * 0.5
                # Pixmaps
                if qpixmap != None:
                    painter.drawPixmap( int( px ), int( py ), qpixmap )
                # Text
                if ( i == self.channel_index and self.state_press == True ):
                    # Bounding Box
                    box = QRect( int( px ), int( py ), int( width ), int( height ) )
                    # Highlight
                    painter.setPen( QtCore.Qt.NoPen )
                    painter.setBrush( QBrush( self.c_dark ) )
                    painter.drawRect( box )
                    # String
                    painter.setBrush( QtCore.Qt.NoBrush )
                    painter.setPen( QPen( self.c_lite, 1, Qt.SolidLine ) )
                    qfont = QFont( "Consolas", 10 )
                    painter.setFont( qfont )
                    painter.drawText( box, Qt.AlignCenter, text )

class Channel_Slider( QWidget ):
    SIGNAL_PA = QtCore.pyqtSignal( float )
    SIGNAL_PB = QtCore.pyqtSignal( float )
    SIGNAL_PC = QtCore.pyqtSignal( float )
    SIGNAL_PD = QtCore.pyqtSignal( float )

    # Init
    def __init__( self, parent ):
        super( Channel_Slider, self ).__init__( parent )
        self.Variables()
    def sizeHint( self ):
        return QtCore.QSize( render_width, 50 )
    # Variables
    def Variables( self ):
        # Widget
        self.ww = 1
        self.hh = 1
        self.w2 = 0.5
        self.h2 = 0.5
        # Original
        self.ex_oa = 0.3
        self.ex_ob = 0.4
        self.ex_oc = 0.6
        self.ex_od = 0.7
        # Event
        self.ex_pa = 0.3
        self.ex_pb = 0.4
        self.ex_pc = 0.6
        self.ex_pd = 0.7
        self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
        self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
        self.mod_3 = QtCore.Qt.ShiftModifier | QtCore.Qt.ControlModifier | QtCore.Qt.AltModifier
        # State
        self.state_press = False
        # Slider
        self.slider_node = None
        self.slider_mode = "LINEAR"  # "LINEAR" "CIRCULAR"
        self.slider_cor = None
        self.slider_order = "ABCD" # "ABCD" "BCDA" "CDAB" "DABC"
        self.slider_gradient = None
        # Colors
        self.c_lite = QColor( "#ffffff" )
        self.c_dark = QColor( "#000000" )
        self.c_white = QColor( "#ffffff" )
        self.c_black = QColor( "#000000" )
        self.brush_black = QBrush( QColor( "#000000" ) )
        self.brush_white = QBrush( QColor( "#ffffff" ) )
    # Relay
    def Set_Size( self, ww, hh ):
        self.ww = int( ww )
        self.hh = int( hh )
        self.h2 = int( hh * 0.5 )
        self.resize( ww, hh )
    def Set_Theme( self, c_lite, c_dark ):
        self.c_lite = QColor( c_lite ); self.c_lite.setAlphaF( 0.9 )
        self.c_dark = QColor( c_dark ); self.c_dark.setAlphaF( 0.9 )
     # Update
    def Update_Mode( self, slider_mode ):
        self.slider_mode = slider_mode
        self.update()
    def Update_Color( self, slider_cor ):
        self.slider_cor = slider_cor
        self.update()
    def Update_Gradient( self, slider_gradient ):
        self.slider_gradient = slider_gradient
        self.update()
    # Mouse Events
    def mousePressEvent( self, event ):
        # Variable
        self.state_press = True
        # Event
        em = event.modifiers()
        eb = event.buttons()
        ex = event.x()
        ey = event.y()
        # LMB
        if ( em == QtCore.Qt.NoModifier and eb == QtCore.Qt.LeftButton ):
            self.slider_node = self.Slider_Node( ex, ey )
            self.Channel_Move( ex, ey )
        if ( em == self.mod_3 and eb == QtCore.Qt.LeftButton ):             self.Channel_Reset()
        # Update
        self.update()
    def mouseMoveEvent( self, event ):
        # Event
        em = event.modifiers()
        eb = event.buttons()
        ex = event.x()
        ey = event.y()
        # Operations
        if ( em == QtCore.Qt.NoModifier and eb == QtCore.Qt.LeftButton ):   self.Channel_Move( ex, ey )
        if ( em == self.mod_3 and eb == QtCore.Qt.LeftButton ):             self.Channel_Reset()
        # Update
        self.update()
    def mouseReleaseEvent( self, event ):
        # Channel Fix
        self.Channel_Cycle()
        # Variables
        self.state_press = False
        self.slider_node = None
        # Update
        self.update()
    # Operation
    def Channel_Move( self, ex, ey ):
        # Variables
        value = ex / self.ww
        # Interaction
        if self.slider_mode == "LINEAR":
            if self.slider_node == "ex_pa":
                self.ex_pa = value
                self.ex_pa = Limit_Range( self.ex_pa, 0, self.ex_pb )
            elif self.slider_node == "ex_pb":
                self.ex_pa = value - self.delta_ab_n
                self.ex_pb = value
                self.ex_pa = Limit_Range( self.ex_pa, 0, self.ex_pc )
                self.ex_pb = Limit_Range( self.ex_pb, 0, self.ex_pc )
            elif self.slider_node == "ex_pc":
                self.ex_pc = value
                self.ex_pd = value + self.delta_cd_n
                self.ex_pc = Limit_Range( self.ex_pc, self.ex_pb, 1 )
                self.ex_pd = Limit_Range( self.ex_pd, self.ex_pb, 1 )
            elif self.slider_node == "ex_pd":
                self.ex_pd = value
                self.ex_pd = Limit_Range( self.ex_pd, self.ex_pc, 1 )
            # Deltas
            self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
            self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
        if self.slider_mode == "CIRCULAR":
            # Neutral
            ex_pa_n = self.ex_pa
            ex_pb_n = self.ex_pb
            ex_pc_n = self.ex_pc
            ex_pd_n = self.ex_pd
            # Left
            ex_pa_l = ex_pa_n - 1
            ex_pb_l = ex_pb_n - 1
            ex_pc_l = ex_pc_n - 1
            ex_pd_l = ex_pd_n - 1
            # Right
            ex_pa_r = ex_pa_n + 1
            ex_pb_r = ex_pb_n + 1
            ex_pc_r = ex_pc_n + 1
            ex_pd_r = ex_pd_n + 1
            # Movement Limits
            if self.slider_order == "ABCD":
                if   self.slider_node == "ex_pa":
                    self.ex_pa = value
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_l, ex_pb_n )
                elif self.slider_node == "ex_pb":
                    self.ex_pb = value
                    self.ex_pa = value - self.delta_ab_n
                    self.ex_pb = Limit_Range( self.ex_pb, ex_pd_l, ex_pc_n )
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_l, ex_pc_n )
                elif self.slider_node == "ex_pc":
                    self.ex_pc = value
                    self.ex_pd = value + self.delta_cd_n
                    self.ex_pc = Limit_Range( self.ex_pc, ex_pb_n, ex_pa_r )
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pb_n, ex_pa_r )
                elif self.slider_node == "ex_pd":
                    self.ex_pd = value
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pc_n, ex_pa_r )
                self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
                self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
            if self.slider_order == "BCDA":
                if   self.slider_node == "ex_pa":
                    self.ex_pa = value
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pb_r )
                elif self.slider_node == "ex_pb":
                    self.ex_pb = value
                    self.ex_pa = value + self.delta_ab_n
                    self.ex_pb = Limit_Range( self.ex_pb, ex_pd_l, ex_pc_n )
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pc_r )
                elif self.slider_node == "ex_pc":
                    self.ex_pc = value
                    self.ex_pd = value + self.delta_cd_n
                    self.ex_pc = Limit_Range( self.ex_pc, ex_pb_n, ex_pa_n )
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pb_n, ex_pa_n )
                elif self.slider_node == "ex_pd":
                    self.ex_pd = value
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pc_n, ex_pa_n )
                self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
                self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
            if self.slider_order == "CDAB":
                if   self.slider_node == "ex_pa":
                    self.ex_pa = value
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pb_n )
                elif self.slider_node == "ex_pb":
                    self.ex_pb = value
                    self.ex_pa = value - self.delta_ab_n
                    self.ex_pb = Limit_Range( self.ex_pb, ex_pd_n, ex_pc_r )
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pc_r )
                elif self.slider_node == "ex_pc":
                    self.ex_pc = value
                    self.ex_pd = value + self.delta_cd_n
                    self.ex_pc = Limit_Range( self.ex_pc, ex_pb_l, ex_pa_n )
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pb_l, ex_pa_n )
                elif self.slider_node == "ex_pd":
                    self.ex_pd = value
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pc_n, ex_pa_n )
                self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
                self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
            if self.slider_order == "DABC":
                if   self.slider_node == "ex_pa":
                    self.ex_pa = value
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pb_n )
                elif self.slider_node == "ex_pb":
                    self.ex_pb = value
                    self.ex_pa = value - self.delta_ab_n
                    self.ex_pb = Limit_Range( self.ex_pb, ex_pd_n, ex_pc_n )
                    self.ex_pa = Limit_Range( self.ex_pa, ex_pd_n, ex_pc_n )
                elif self.slider_node == "ex_pc":
                    self.ex_pc = value
                    self.ex_pd = value - self.delta_cd_n
                    self.ex_pc = Limit_Range( self.ex_pc, ex_pb_n, ex_pa_r )
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pb_l, ex_pa_n )
                elif self.slider_node == "ex_pd":
                    self.ex_pd = value
                    self.ex_pd = Limit_Range( self.ex_pd, ex_pc_l, ex_pa_n )
                self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
                self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
        # Emit
        self.SIGNAL_PA.emit( self.ex_pa )
        self.SIGNAL_PB.emit( self.ex_pb )
        self.SIGNAL_PC.emit( self.ex_pc )
        self.SIGNAL_PD.emit( self.ex_pd )
    def Channel_Cycle( self ):
        # Position
        self.ex_pa = Limit_Cycle( self.ex_pa, 0, 1 )
        self.ex_pb = Limit_Cycle( self.ex_pb, 0, 1 )
        self.ex_pc = Limit_Cycle( self.ex_pc, 0, 1 )
        self.ex_pd = Limit_Cycle( self.ex_pd, 0, 1 )
        self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
        self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
        # Point Order
        self.slider_order = self.Slider_Order()
        # Emit
        self.SIGNAL_PA.emit( self.ex_pa )
        self.SIGNAL_PB.emit( self.ex_pb )
        self.SIGNAL_PC.emit( self.ex_pc )
        self.SIGNAL_PD.emit( self.ex_pd )
    def Channel_Reset( self ):
        # Variables
        self.ex_pa = 0.3
        self.ex_pb = 0.4
        self.ex_pc = 0.6
        self.ex_pd = 0.7
        self.delta_ab_n = abs( self.ex_pa - self.ex_pb )
        self.delta_cd_n = abs( self.ex_pd - self.ex_pc )
        # Signals
        self.SIGNAL_PA.emit( self.ex_pa )
        self.SIGNAL_PB.emit( self.ex_pb )
        self.SIGNAL_PC.emit( self.ex_pc )
        self.SIGNAL_PD.emit( self.ex_pd )
    # Slider
    def Slider_Node( self, ex, ey ):
        # Variable
        slider_node = None
        dist = 20
        # Distance
        w_pa = int( self.ex_pa * self.ww )
        w_pb = int( self.ex_pb * self.ww )
        w_pc = int( self.ex_pc * self.ww )
        w_pd = int( self.ex_pd * self.ww )
        # Delta
        d_pa_n = abs( ex - w_pa )
        d_pb_n = abs( ex - w_pb )
        d_pc_n = abs( ex - w_pc )
        d_pd_n = abs( ex - w_pd )
        # Delta Left
        d_pa_l = abs( ex - ( w_pa - self.ww ) )
        d_pb_l = abs( ex - ( w_pb - self.ww ) )
        d_pc_l = abs( ex - ( w_pc - self.ww ) )
        d_pd_l = abs( ex - ( w_pd - self.ww ) )
        # Delta Right
        d_pa_r = abs( ex - ( w_pa + self.ww ) )
        d_pb_r = abs( ex - ( w_pb + self.ww ) )
        d_pc_r = abs( ex - ( w_pc + self.ww ) )
        d_pd_r = abs( ex - ( w_pd + self.ww ) )
        # Minimum Distance
        min_pa = min( d_pa_n, d_pa_l, d_pa_r )
        min_pb = min( d_pb_n, d_pb_l, d_pb_r )
        min_pc = min( d_pc_n, d_pc_l, d_pc_r )
        min_pd = min( d_pd_n, d_pd_l, d_pd_r )
        # TOP SIDE
        if ey <= self.h2:
            if min_pb != min_pc:
                node = min( d_pb_n, d_pc_n )
                if d_pb_n <= dist and node == d_pb_n: slider_node = "ex_pb"
                if d_pc_n <= dist and node == d_pc_n: slider_node = "ex_pc"
            elif min_pb == min_pc:
                if min_pb <= dist and ex <= w_pb: slider_node = "ex_pb"
                if min_pc <= dist and ex >= w_pc: slider_node = "ex_pc"
        # BOTTOM SIDE
        else:
            if min_pa != min_pc:
                node = min( d_pa_n, d_pd_n )
                if d_pa_n <= dist and node == d_pa_n: slider_node = "ex_pa"
                if d_pd_n <= dist and node == d_pd_n: slider_node = "ex_pd"
            elif min_pa == min_pd:
                if min_pa <= dist and ex <= w_pa: slider_node = "ex_pa"
                if min_pd <= dist and ex >= w_pd: slider_node = "ex_pd"
        return slider_node
    def Slider_Order( self ):
        # Neutral
        ex_pa_n = round( self.ex_pa, 6 )
        ex_pb_n = round( self.ex_pb, 6 )
        ex_pc_n = round( self.ex_pc, 6 )
        ex_pd_n = round( self.ex_pd, 6 )
        # Left
        ex_pa_l = round( ex_pa_n - 1.0, 6 )
        ex_pb_l = round( ex_pb_n - 1.0, 6 )
        ex_pc_l = round( ex_pc_n - 1.0, 6 )
        ex_pd_l = round( ex_pd_n - 1.0, 6 )
        # Right
        ex_pa_r = round( ex_pa_n + 1.0, 6 )
        ex_pb_r = round( ex_pb_n + 1.0, 6 )
        ex_pc_r = round( ex_pc_n + 1.0, 6 )
        ex_pd_r = round( ex_pd_n + 1.0, 6 )

        # Sorting
        l = [ ( ex_pa_n, "A" ), ( ex_pb_n, "B" ), ( ex_pc_n, "C" ), ( ex_pd_n, "D" ) ]
        l.sort()
        so = l[0][1] + l[1][1] + l[2][1] + l[3][1]
        # Fix Sort Errors
        if so not in [ "ABCD", "BCDA", "CDAB", "DABC" ]:
            # Variables
            ca = ex_pa_n in [ ex_pb_l,ex_pc_l,ex_pd_l, ex_pb_n,ex_pc_n,ex_pd_n, ex_pb_r,ex_pc_r,ex_pd_r ]
            cb = ex_pb_n in [ ex_pa_l,ex_pc_l,ex_pd_l, ex_pa_n,ex_pc_n,ex_pd_n, ex_pa_r,ex_pc_r,ex_pd_r ]
            cc = ex_pc_n in [ ex_pa_l,ex_pb_l,ex_pd_l, ex_pa_n,ex_pb_n,ex_pd_n, ex_pa_r,ex_pb_r,ex_pd_r ]
            cd = ex_pd_n in [ ex_pa_l,ex_pb_l,ex_pc_l, ex_pa_n,ex_pb_n,ex_pc_n, ex_pa_r,ex_pb_r,ex_pc_r ]

            # 4 letters ( reset )
            if   ( ca == cb == cc == cd == True ):
                so = "ABCD"
            # 3 letters
            elif ( ca == False ):
                if so[0] == "A": so = "ABCD"
                if so[1] == "A": so = "DABC"
                if so[2] == "A": so = "CDAB"
                if so[3] == "A": so = "BCDA"
            elif ( cb == False ):
                if so[0] == "B": so = "BCDA"
                if so[1] == "B": so = "ABCD"
                if so[2] == "B": so = "DABC"
                if so[3] == "B": so = "CDAB"
            elif ( cc == False ):
                if so[0] == "C": so = "CDAB"
                if so[1] == "C": so = "BCDA"
                if so[2] == "C": so = "ABCD"
                if so[3] == "C": so = "DABC"
            elif ( cd == False ):
                if so[0] == "D": so = "DABC"
                if so[1] == "D": so = "CDAB"
                if so[2] == "D": so = "BCDA"
                if so[3] == "D": so = "ABCD"
        return so
    # Painter
    def paintEvent( self, event ):
        # Variables
        ww = self.ww
        hh = self.hh
        h2 = self.h2
        d = 3
        h2t = h2 - d
        h2b = h2 + d
        delta = 4
        k = 255
        line_size = 2
        line_back = 4
        # Points
        px = int( 1 )
        py = int( 10 )
        dw = int( self.ww - ( 2 * px ) )
        dh = int( self.hh - ( 2 * py ) )

        # Markers Neutral
        ex_pa_n = int( self.ex_pa * ww )
        ex_pb_n = int( self.ex_pb * ww )
        ex_pc_n = int( self.ex_pc * ww )
        ex_pd_n = int( self.ex_pd * ww )
        check_full = ex_pa_n == ex_pb_n == ex_pc_n == ex_pd_n
        # offset
        ex_pa_n -= 2
        ex_pb_n -= 2
        ex_pc_n += 2
        ex_pd_n += 2
        if self.slider_mode == "LINEAR":
            ex_pa_n = Limit_Range( ex_pa_n, 0, ww )
            ex_pb_n = Limit_Range( ex_pb_n, 0, ww )
            ex_pc_n = Limit_Range( ex_pc_n, 0, ww )
            ex_pd_n = Limit_Range( ex_pd_n, 0, ww )
        if self.slider_mode == "CIRCULAR":
            # Markers Left
            ex_pa_l = int( ex_pa_n - ww )
            ex_pb_l = int( ex_pb_n - ww )
            ex_pc_l = int( ex_pc_n - ww )
            ex_pd_l = int( ex_pd_n - ww )
            # Markers Right
            ex_pa_r = int( ex_pa_n + ww )
            ex_pb_r = int( ex_pb_n + ww )
            ex_pc_r = int( ex_pc_n + ww )
            ex_pd_r = int( ex_pd_n + ww )

        # Painter
        painter = QPainter( self )
        painter.setRenderHint( QtGui.QPainter.Antialiasing, True )

        # Channels
        if self.slider_gradient != None:
            painter.setPen( QPen( self.c_lite, line_size, Qt.SolidLine, Qt.SquareCap, Qt.MiterJoin ) )
            painter.setBrush( QtCore.Qt.NoBrush )

            # Gradient
            grad = QLinearGradient( px, py, dw, dh )
            num = len( self.slider_gradient )
            for i in range( 0, num ):
                loc = round( i / ( num - 1 ), 3 )
                r = int( self.slider_gradient[i][0] * k )
                g = int( self.slider_gradient[i][1] * k )
                b = int( self.slider_gradient[i][2] * k )
                color = QColor( r, g, b, k )
                grad.setColorAt( loc, color )
            painter.setBrush( QBrush( grad ) )
            painter.setPen( QtCore.Qt.NoPen )
            square = QPolygon( [
                QPoint( int( px ),  int( py ) ),
                QPoint( int( dw ), int( py ) ),
                QPoint( int( dw ), int( py+dh ) ),
                QPoint( int( px ),  int( py+dh ) ),
                ] )
            painter.drawPolygon( square )

            # Variables
            ht = 6
            hb = self.hh - ht
            # Triangles
            painter.setBrush( QtCore.Qt.NoBrush )
            if self.slider_mode == "LINEAR":
                tl = None
                tn = QPolygon( [
                    QPoint( ex_pb_n, hb ),
                    QPoint( ex_pa_n, hb ),
                    QPoint( ex_pb_n, ht ),
                    QPoint( ex_pb_n, hb ),

                    QPoint( ex_pc_n, hb ),
                    QPoint( ex_pc_n, ht ),
                    QPoint( ex_pd_n, hb ),
                    QPoint( ex_pc_n, hb ),
                    ] )
                tr = None
            if self.slider_mode == "CIRCULAR":
                if check_full == True:
                    tl = None
                    tn = QPolygon( [
                        QPoint( 0, hb ),
                        QPoint( ex_pa_n, hb ),
                        QPoint( ex_pb_n, ht ),
                        QPoint( ex_pc_n, ht ),
                        QPoint( ex_pd_n, hb ),
                        QPoint( ww, hb ),
                        ] )
                    tr = None
                elif self.slider_order == "ABCD":
                    tl = QPolygon( [
                        QPoint( ex_pb_l, hb ),
                        QPoint( ex_pa_l, hb ),
                        QPoint( ex_pb_l, ht ),
                        QPoint( ex_pb_l, hb ),

                        QPoint( ex_pc_l, hb ),
                        QPoint( ex_pc_l, ht ),
                        QPoint( ex_pd_l, hb ),
                        QPoint( ex_pc_l, hb ),
                        ] )
                    tn = QPolygon( [
                        QPoint( ex_pb_n, hb ),
                        QPoint( ex_pa_n, hb ),
                        QPoint( ex_pb_n, ht ),
                        QPoint( ex_pb_n, hb ),

                        QPoint( ex_pc_n, hb ),
                        QPoint( ex_pc_n, ht ),
                        QPoint( ex_pd_n, hb ),
                        QPoint( ex_pc_n, hb ),
                        ] )
                    tr = QPolygon( [
                        QPoint( ex_pb_r, hb ),
                        QPoint( ex_pa_r, hb ),
                        QPoint( ex_pb_r, ht ),
                        QPoint( ex_pb_r, hb ),

                        QPoint( ex_pc_r, hb ),
                        QPoint( ex_pc_r, ht ),
                        QPoint( ex_pd_r, hb ),
                        QPoint( ex_pc_r, hb ),
                        ] )
                elif self.slider_order == "BCDA":
                    tl = QPolygon( [
                        QPoint( ex_pb_n, hb ),
                        QPoint( ex_pa_l, hb ),
                        QPoint( ex_pb_n, ht ),
                        QPoint( ex_pb_n, hb ),

                        QPoint( ex_pc_n, hb ),
                        QPoint( ex_pc_n, ht ),
                        QPoint( ex_pd_n, hb ),
                        QPoint( ex_pc_n, hb ),
                        ] )
                    tn = None
                    tr = QPolygon( [
                        QPoint( ex_pb_r, hb ),
                        QPoint( ex_pa_n, hb ),
                        QPoint( ex_pb_r, ht ),
                        QPoint( ex_pb_r, hb ),

                        QPoint( ex_pc_r, hb ),
                        QPoint( ex_pc_r, ht ),
                        QPoint( ex_pd_r, hb ),
                        QPoint( ex_pc_r, hb ),
                        ] )
                elif self.slider_order == "CDAB":
                    tl = QPolygon( [
                        QPoint( ex_pb_l, hb ),
                        QPoint( ex_pa_l, hb ),
                        QPoint( ex_pb_l, ht ),
                        QPoint( ex_pb_l, hb ),

                        QPoint( ex_pc_n, hb ),
                        QPoint( ex_pc_n, ht ),
                        QPoint( ex_pd_n, hb ),
                        QPoint( ex_pc_n, hb ),
                        ] )
                    tn = None
                    tr = QPolygon( [
                        QPoint( ex_pb_n, hb ),
                        QPoint( ex_pa_n, hb ),
                        QPoint( ex_pb_n, ht ),
                        QPoint( ex_pb_n, hb ),

                        QPoint( ex_pc_r, hb ),
                        QPoint( ex_pc_r, ht ),
                        QPoint( ex_pd_r, hb ),
                        QPoint( ex_pc_r, hb ),
                        ] )
                elif self.slider_order == "DABC":
                    tl = QPolygon( [
                        QPoint( ex_pb_l, hb ),
                        QPoint( ex_pa_l, hb ),
                        QPoint( ex_pb_l, ht ),
                        QPoint( ex_pb_l, hb ),

                        QPoint( ex_pc_l, hb ),
                        QPoint( ex_pc_l, ht ),
                        QPoint( ex_pd_n, hb ),
                        QPoint( ex_pc_l, hb ),
                        ] )
                    tn = None
                    tr = QPolygon( [
                        QPoint( ex_pb_n, hb ),
                        QPoint( ex_pa_n, hb ),
                        QPoint( ex_pb_n, ht ),
                        QPoint( ex_pb_n, hb ),

                        QPoint( ex_pc_n, hb ),
                        QPoint( ex_pc_n, ht ),
                        QPoint( ex_pd_r, hb ),
                        QPoint( ex_pc_n, hb ),
                        ] )
            painter.setPen( QPen( self.c_black, line_back, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin ) )
            if tl != None:  painter.drawPolygon( tl )
            if tn != None:  painter.drawPolygon( tn )
            if tr != None:  painter.drawPolygon( tr )
            painter.setPen( QPen( self.c_white, line_size, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin ) )
            if tl != None:  painter.drawPolygon( tl )
            if tn != None:  painter.drawPolygon( tn )
            if tr != None:  painter.drawPolygon( tr )
            # Marker for Color
            if self.slider_cor != None:
                # Cursor
                value = int( self.slider_cor * ww )
                bl = value - 3
                br = value + 3
                wl = value - 1
                wr = value + 1
                top1 = 0
                bot1 = self.hh
                top2 = 1
                bot2 = self.hh - 1
                # Black Square
                painter.setPen( QtCore.Qt.NoPen )
                painter.setBrush( self.brush_black )
                black = QPolygon( [
                    QPoint( int( bl ), int( top1 ) ),
                    QPoint( int( bl ), int( bot1 ) ),
                    QPoint( int( br ), int( bot1 ) ),
                    QPoint( int( br ), int( top1 ) ),
                    ] )
                painter.drawPolygon( black )
                # White Square
                painter.setPen( QtCore.Qt.NoPen )
                painter.setBrush( self.brush_white )
                square = QPolygon( [
                    QPoint( int( wl ), int( top2 ) ),
                    QPoint( int( wl ), int( bot2 ) ),
                    QPoint( int( wr ), int( bot2 ) ),
                    QPoint( int( wr ), int( top2 ) ),
                    ] )
                painter.drawPolygon( square )

#endregion
