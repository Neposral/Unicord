﻿using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using DSharpPlus.Entities;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Navigation;

namespace Unicord.Universal.Controls
{
    public sealed partial class UserFlyout : Flyout
    {
        public object DataContext
        {
            get => (Content as FrameworkElement).DataContext;
            set => (Content as FrameworkElement).DataContext = value;
        }

        public UserFlyout()
        {
            InitializeComponent();
        }

        private void Button_Click(object sender, RoutedEventArgs e)
        {
            Hide();
            Window.Current.Content.FindChild<MainPage>()
                .ShowUserOverlay(DataContext as DiscordUser, true);
        }
    }
}
